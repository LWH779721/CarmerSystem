#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtpudpv4transmitter.h"
#include "rtpipv4address.h"
#include "rtptimeutilities.h"
#include "rtppacket.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <alsa/asoundlib.h>

#include <faac.h>

#include <stdlib.h>
#include <iostream>

#define SSRC           100
typedef unsigned long   ULONG;
typedef unsigned int    UINT;
typedef unsigned char   BYTE;

using namespace jrtplib;

typedef struct PCMContainer {
	snd_pcm_t *handle;              /*need to set*/
	snd_output_t *log;
	snd_pcm_uframes_t chunk_size;   /*auto calc*/
	snd_pcm_uframes_t buffer_size;  /*auto calc*/
	snd_pcm_format_t format;        /*need to set*/
	uint16_t channels;              /*need to set*/
	size_t chunk_bytes;
	size_t bits_per_sample;
	size_t bits_per_frame;
	size_t sample_rate;	            /*need to set*/

	uint8_t *data_buf;
} PCMContainer_t;

ssize_t uAlsaReadPcm(PCMContainer_t *sndPCM, size_t rCount)
{
    ssize_t r;
    size_t result = 0;
    size_t count = rCount;
    uint8_t *data = sndPCM->data_buf;
    int res;

    if (count != sndPCM->chunk_size)
    {
        count = sndPCM->chunk_size;
    }

    while (count > 0)
    {
        r = snd_pcm_readi(sndPCM->handle, data, count);

        if (r == -EAGAIN || (r >= 0 && (size_t)r < count))
        {
            snd_pcm_wait(sndPCM->handle, 1000);
        }
        else if (r == -EPIPE)
        {
            snd_pcm_prepare(sndPCM->handle);
            printf("<<<<<<<<<<<<<<< Buffer Underrun >>>>>>>>>>>>>>>\n");
        }
        else if (r == -ESTRPIPE)
        {
            printf("<<<<<<<<<<<<<<<Read Need suspend >>>>>>>>>>>>>>>\n");

            while ((res = snd_pcm_resume(sndPCM->handle)) == -EAGAIN)
                sleep(1);/* wait until suspend flag is released */
            if (res < 0) 
            {
                if ((res = snd_pcm_prepare(sndPCM->handle)) < 0) 
                {
                    printf("resume read from suspend fail\n");
                }
            }
        }
        else if (r < 0)
        {
            printf("Error snd_pcm_readi: [%s]\n", snd_strerror(r));
            return -1;
        }

        if (r > 0)
        {
            result += r;
            count -= r;
            data += r * sndPCM->bits_per_frame / 8;
        }
    }
    return rCount;
}

int uAlsaSetSwParamsForMicRecord(PCMContainer_t *pPCMParams, snd_pcm_uframes_t val)
{
    int ret;
    snd_pcm_sw_params_t *ptr = NULL;

    snd_pcm_sw_params_alloca(&ptr);
    if (NULL == ptr)
    {
        printf("snd_pcm_sw_params_malloc error!\n");
        goto MALLOC_PARAMS_ERR;
    }

    ret = snd_pcm_sw_params_current(pPCMParams->handle, ptr);
    if (ret < 0)
    {
        printf("snd_pcm_sw_params_current error ret:[%d]!\n", ret);
        goto GET_PARAMS_ERR;
    }

    ret = snd_pcm_sw_params_set_avail_min(pPCMParams->handle, ptr, val);
    if (ret < 0)
    {
        printf("snd_pcm_sw_params_set_start_threshold error ret:[%d]!\n", ret);
        goto GET_PARAMS_ERR;
    }

    ret = snd_pcm_sw_params(pPCMParams->handle, ptr);
    if (ret < 0)
    {
        printf("snd_pcm_sw_params error ret:[%d]!\n", ret);
        goto GET_PARAMS_ERR;
    }

    return 0;

GET_PARAMS_ERR:
    snd_pcm_sw_params_free(ptr);
MALLOC_PARAMS_ERR:
    return -1;
}

int uAlsaSetHwParamsForMicRecord(PCMContainer_t *pPcmParams, uint32_t bufferTime, uint32_t periodTime)
{
    snd_pcm_hw_params_t *pHwPcmParams;
    uint32_t exactRate;
    uint32_t maxBufferTime;
    int err;

    /* Allocate the snd_pcm_hw_params_t structure on the stack. */
    snd_pcm_hw_params_alloca(&pHwPcmParams);

    /* Fill it with default values */
    err = snd_pcm_hw_params_any(pPcmParams->handle, pHwPcmParams);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_any : %s\n",snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    /* Interleaved mode */
    err = snd_pcm_hw_params_set_access(pPcmParams->handle, pHwPcmParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_access : %s\n",snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    /* Set sample format */
    err = snd_pcm_hw_params_set_format(pPcmParams->handle, pHwPcmParams, pPcmParams->format);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_format : %s\n",snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    /* Set number of channels */
    err = snd_pcm_hw_params_set_channels(pPcmParams->handle, pHwPcmParams, pPcmParams->channels);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_channels : %s\n",snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    /* Set sample rate. If the exact rate is not supported */
    /* by the hardware, use nearest possible rate.         */
    exactRate = (pPcmParams->sample_rate);
    err = snd_pcm_hw_params_set_rate_near(pPcmParams->handle, pHwPcmParams, &exactRate, 0);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_rate_near : %s\n",snd_strerror(err));
        goto ERR_SET_PARAMS;
    }
    if ((pPcmParams->sample_rate) != exactRate)
    {
        printf("The rate %d Hz is not supported by your hardware.\n ==> Using %d Hz instead.\n",
        (pPcmParams->sample_rate), exactRate);
    }

    err = snd_pcm_hw_params_get_buffer_time_max(pHwPcmParams, &maxBufferTime, 0);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_get_buffer_time_max : %s\n",
                    snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    printf("snd_pcm_hw_params_get_buffer_time_max : %ul (us)\n",maxBufferTime);

    if (bufferTime > maxBufferTime)
    {
        bufferTime = maxBufferTime;
    }

    err = snd_pcm_hw_params_set_buffer_time_near(pPcmParams->handle, pHwPcmParams, &bufferTime, 0);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_buffer_time_near : %s\n",
                    snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    err = snd_pcm_hw_params_set_period_time_near(pPcmParams->handle, pHwPcmParams, &periodTime, 0);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params_set_period_time_near : %s\n",
                    snd_strerror(err));
        goto ERR_SET_PARAMS;
    }

    /* Set hw params */
    err = snd_pcm_hw_params(pPcmParams->handle, pHwPcmParams);
    if (err < 0)
    {
        printf("Error snd_pcm_hw_params: %s at line->%d\n",snd_strerror(err),__LINE__);
        goto ERR_SET_PARAMS;
    }

    snd_pcm_hw_params_get_period_size(pHwPcmParams, &pPcmParams->chunk_size, 0);
    snd_pcm_hw_params_get_buffer_size(pHwPcmParams, &pPcmParams->buffer_size);
    if (pPcmParams->chunk_size == pPcmParams->buffer_size)
    {
        printf("Can't use period equal to buffer size (%lu == %lu)\n",
                    pPcmParams->chunk_size, pPcmParams->buffer_size);
        goto ERR_SET_PARAMS;
    }

    printf("chunk_size is %lu, buffer size is %lu\n",
                pPcmParams->chunk_size, pPcmParams->buffer_size);

    /*bits per sample = bits depth*/
    pPcmParams->bits_per_sample = snd_pcm_format_physical_width(pPcmParams->format);

    /*bits per frame = bits depth * channels*/
    pPcmParams->bits_per_frame = pPcmParams->bits_per_sample * (pPcmParams->channels);

    /*chunk byte is a better size for each write or read for alsa*/
    pPcmParams->chunk_bytes = pPcmParams->chunk_size * pPcmParams->bits_per_frame / 8;

    /* Allocate audio data buffer */
    pPcmParams->data_buf = (uint8_t *)malloc(pPcmParams->chunk_bytes);
    if (!pPcmParams->data_buf)
    {
        printf("Error malloc: [data_buf] at line-> %d\n",__LINE__);
        goto ERR_SET_PARAMS;
    }

    return 0;

    ERR_SET_PARAMS:
    if(NULL != pPcmParams->data_buf)
    {
        free(pPcmParams->data_buf);
        pPcmParams->data_buf = NULL;
    }
    snd_pcm_close(pPcmParams->handle);
    pPcmParams->handle = NULL;
    return -1;
}

int record_init(const char *device, RTPSession& session)
{
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *params;
    PCMContainer_t *pcmParams;
    int    rc;
    
    pcmParams = (PCMContainer_t *)malloc(sizeof(PCMContainer_t));
    if (NULL == pcmParams)
    {
        printf("malloc pcmParams failed!\r\n");
    	return -1;  
    }

    memset(pcmParams, 0x00, sizeof(PCMContainer_t));
    
    pcmParams->format = SND_PCM_FORMAT_S16_LE;
    pcmParams->handle = handle;
    pcmParams->sample_rate = 44100;
    pcmParams->channels = 1;
    
    rc = snd_pcm_open(&pcmParams->handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0)
    {
        perror("open device failed:\n");
        return -1;
    }
    
    rc = uAlsaSetHwParamsForMicRecord(pcmParams, 500*1000, 10*1000);
    if (0 != rc)
    {
        printf("uAlsaSetHwParamsForMicRecord failed! line:%d.\r\n",__LINE__);
		free(pcmParams); 
		return -1; 
    }

    rc = uAlsaSetSwParamsForMicRecord(pcmParams, pcmParams->chunk_size);
    if (0 != rc)
    {
        printf("uAlsaSetSwParamsForMicRecord failed! line:%d.\r\n",__LINE__);
		free(pcmParams); 
		return -1; 
    }

    UINT nPCMBitSize = 16;      // 单样本位数
    ULONG nInputSamples = 0;
    ULONG nMaxOutputBytes = 0;
 
    int nRet;
    faacEncHandle hEncoder;
    faacEncConfigurationPtr pConfiguration;
 
    int nBytesRead;
    int nPCMBufferSize;
    BYTE* pbPCMBuffer;
    BYTE* pbAACBuffer;
	
    hEncoder = faacEncOpen(pcmParams->sample_rate, pcmParams->channels, &nInputSamples, &nMaxOutputBytes);
    if(hEncoder == NULL)
    {
        printf("[ERROR] Failed to call faacEncOpen()\n");
        return -1;
    }

    nPCMBufferSize = nInputSamples * nPCMBitSize / 8;
    pbPCMBuffer = new BYTE [nPCMBufferSize];
    pbAACBuffer = new BYTE [nMaxOutputBytes];
 
    pConfiguration = faacEncGetCurrentConfiguration(hEncoder);
	
	pConfiguration->aacObjectType = LOW;    //LC编码
	pConfiguration->mpegVersion = MPEG4;  //
	pConfiguration->useTns   = 1 ;                   //时域噪音控制,大概就是消爆音
	pConfiguration->allowMidside = 0 ;            //
	pConfiguration->bitRate  = 0;
	pConfiguration->bandWidth  = 0 ;              //频宽
	pConfiguration->outputFormat = 1;  //输出是否包含ADTS头
	pConfiguration->inputFormat = FAAC_INPUT_16BIT;
	pConfiguration->quantqual = 100 ;

    nRet = faacEncSetConfiguration(hEncoder, pConfiguration);
	char sendbuf[1500];
	int status;
	sendbuf[0] = 0x00;
	sendbuf[1] = 0x10;
	
	RTPTime delay = RTPTime(0.010);
    while (1)
    {
        rc = uAlsaReadPcm(pcmParams, pcmParams->chunk_size);
        if (-1 == rc)
        {
            printf("%s,line:%d.read err.\r\n",__FUNCTION__,__LINE__);
            continue;
        }
        #if 1
		nRet = faacEncEncode(
            hEncoder, (int*) pcmParams->data_buf, pcmParams->chunk_size, pbAACBuffer, nMaxOutputBytes);
 
		if (nRet == 0)
		{
			continue;
		}
		
		printf("%d\n", nRet);
		sendbuf[2] = ((nRet - 7) & 0x1fe0) >> 5;
		sendbuf[3] = ((nRet - 7) & 0x1f) << 3;
		memcpy(sendbuf + 4, pbAACBuffer + 7, nRet - 7);
	
		status = session.SendPacket((void *)sendbuf, nRet - 3,97,true, 10);
		if (status < 0)
		{
			std::cerr << RTPGetErrorString(status) << std::endl;
			exit(-1);
		}
		#else
		memcpy(sendbuf, pcmParams->data_buf, pcmParams->chunk_bytes);
	
		status = session.SendPacket((void *)sendbuf, pcmParams->chunk_bytes,97,true, 10);
		if (status < 0)
		{
			std::cerr << RTPGetErrorString(status) << std::endl;
			exit(-1);
		}
		#endif
		RTPTime::Wait(delay);
    }
    
	nRet = faacEncClose(hEncoder);
 
    delete[] pbPCMBuffer;
    delete[] pbAACBuffer;  
}

int main(int argc, char** argv)
{
	RTPSession session;
	RTPSessionParams sessionparams;
	RTPUDPv4TransmissionParams transparams;
	
	sessionparams.SetOwnTimestampUnit(1.0/100.0);
	transparams.SetPortbase(8000);

	int status = session.Create(sessionparams,&transparams);
	if (status < 0)
	{
		std::cerr << RTPGetErrorString(status) << std::endl;
		exit(-1);
	}

	uint8_t localip[]={ 192,168, 70, 45};
	RTPIPv4Address addr(localip,8000);

	status = session.AddDestination(addr);
	if (status < 0)
	{
		std::cerr << RTPGetErrorString(status) << std::endl;
		exit(-1);
	}

	session.SetDefaultPayloadType(97);
	session.SetDefaultMark(false);
	session.SetDefaultTimestampIncrement(10);

	RTPTime starttime = RTPTime::CurrentTime();

	char* nalu_payload;
	unsigned int timestamp_increse=0,ts_current=0;
	
	record_init("hw:0,3", session);
		
	printf("over\n");
	RTPTime delay = RTPTime(10.0);
	session.BYEDestroy(delay,"Time's up",9);
}







