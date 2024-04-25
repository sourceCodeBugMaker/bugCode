#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <time.h>
#include <libavcodec/avcodec.h> 
#include <libavdevice/avdevice.h> 
#include <libavformat/avformat.h> 
#include <libavfilter/avfilter.h> 
#include <libavutil/avutil.h> 
#include <libswscale/swscale.h>
#include "ringBuffer.h"
#include "stream_dump.h"
#include "stream_pull.h"

#define WRITE_MAX_FRAME 1000//在转到mp4文件的帧数达到就结束


static FILE *fp = NULL;
#define recordFileName "tmp.mp4"

typedef struct{
    int spsLen;
    char spsData[256];
    int ppsLen;
    char ppsData[128];
}CodecparExtradataPayload_ST;

static bool s_blStreamIsDumping = false;            //线程控制
static AVFormatContext* s_pstFormatCtx = NULL;      //mp4上下文，用于写mp4文件
static int s_s32VideoStreamdIdx = -1;                    //mp4上下文中视频流的索引位置   
CodecparExtradataPayload_ST s_stExtrPayload;


static int s_s32FrameCnt = 0;                        //累积写入到MP4中的帧计数:I帧 + P帧




static void writeVideo(bool isKeyFrame, Nalu_ST *pstNalu)
{
    
    s_s32FrameCnt++;

    // Init packet
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.stream_index = s_s32VideoStreamdIdx;

    pkt.data = pstNalu->frameData;
    pkt.size = pstNalu->size + 4; //4字节的NALU length

    // Wait for key frame_count
    pkt.dts = s_s32FrameCnt * 90000 / 30;
    pkt.pts = pkt.dts;
    pkt.duration = 90000 / 30;
    pkt.flags |= (isKeyFrame ? AV_PKT_FLAG_KEY : 0);

    int ret = av_interleaved_write_frame(s_pstFormatCtx, &pkt);
    if (ret != 0) 
    {
        printf("write interleaved frame failed\n");
    }
}


static int mp4ContextInit(void)
{
    int ret = 0;
    /*mp4视频流编码器参数中的extradata,包含s_stExtrPayload中的sps和pps信息,按照avcc格式封装
 mp4封装视频必须要先封装此数据,否则无法正常解码播放*/
    uint8_t *pu8CodecExtradata;
    pu8CodecExtradata = (uint8_t *)malloc(1024);
    if (NULL == pu8CodecExtradata)
    {
        printf("malloc failed\n");
        return -1;
    }
    memset(pu8CodecExtradata, 0, 1024);

    ret = avformat_alloc_output_context2(&s_pstFormatCtx, NULL, "mp4", recordFileName);
    if (ret < 0)
    {
        printf("avformat_alloc_output_context2 failed\n");
        free(pu8CodecExtradata);
        return -1;
    }
    AVOutputFormat *ofmt = s_pstFormatCtx->oformat;
    if (!(ofmt->flags & AVFMT_NOFILE)) 
    {
        ret = avio_open(&s_pstFormatCtx->pb, recordFileName, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            printf("Could not open output file %s", recordFileName);
            free(pu8CodecExtradata);
            avformat_free_context(s_pstFormatCtx);
            return -1;
        }
    }

    /*添加视频流,并配置初始化参数*/
    AVStream *outStream = avformat_new_stream(s_pstFormatCtx, NULL);
    outStream->codecpar->codec_id   = AV_CODEC_ID_H264;
    outStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codecpar->width      = 3840;
    outStream->codecpar->height     = 2160;
    //outStream->id                   = s_pstFormatCtx->nb_streams - 1;
    outStream->time_base.num        = 1;
    outStream->time_base.den        = 90000;

    //按照avcc格式拼接extradata:sps和pps数据
    memset(pu8CodecExtradata, 0, sizeof(pu8CodecExtradata));
    uint8_t avccHead[7] = {0x01, 0x64, 0x00, 0x28, 0xff, 0xe1, 0x00};
    int hLen = sizeof(avccHead);
    memcpy(pu8CodecExtradata, avccHead, hLen);                                                                       //hlen bytes
    pu8CodecExtradata[hLen] = s_stExtrPayload.spsLen;                                                                //1 byte
    memcpy(pu8CodecExtradata + hLen + 1, s_stExtrPayload.spsData, s_stExtrPayload.spsLen);                           //s_stExtrPayload.spsLen bytes
    pu8CodecExtradata[s_stExtrPayload.spsLen + hLen + 1] = 0x01;                                                     //1 byte
    pu8CodecExtradata[s_stExtrPayload.spsLen + hLen + 2] = 0x00;                                                     //1 byte
    pu8CodecExtradata[s_stExtrPayload.spsLen + hLen + 3] = s_stExtrPayload.ppsLen;                                   //1 byte
    memcpy(pu8CodecExtradata + s_stExtrPayload.spsLen + hLen + 4, s_stExtrPayload.ppsData, s_stExtrPayload.ppsLen);  //s_stExtrPayload ppsLen bytes
    int avccLen = hLen + s_stExtrPayload.spsLen + s_stExtrPayload.ppsLen + 4;
    outStream->codecpar->extradata  = pu8CodecExtradata; //此后pu8CodecExtradata由avformat_free_context函数统一释放
    outStream->codecpar->extradata_size = avccLen;
    s_s32VideoStreamdIdx = outStream->index; //在把流写进MP4时需要
    
    av_dump_format(s_pstFormatCtx, 0, recordFileName, 1);

    ret = avformat_write_header(s_pstFormatCtx, NULL);
    if (ret < 0) 
    {
        printf("write header failed\n");
        avformat_free_context(s_pstFormatCtx);
        return -1;
    }

    return 0;
}


void mp4ContextFinish(void)
{
    if (s_pstFormatCtx != NULL)
    {
        av_interleaved_write_frame(s_pstFormatCtx, NULL);
        av_write_trailer(s_pstFormatCtx);
        avformat_free_context(s_pstFormatCtx);
        s_pstFormatCtx = NULL;
        s_s32VideoStreamdIdx = -1;
        s_s32FrameCnt = 0;
    }
}

void *streamDumpProc(void *argc)
{
    prctl(PR_SET_NAME, "streamDumpProc");
    if (pthread_detach(pthread_self()))
    {
        printf("pthrad detach error\n");
        return NULL;
    }

    int ret = 0;
    int s32SpsPpsFlag = 0;
    time_t startTime = 0;
    int dropFrameNum = 0;
    Nalu_ST stNalu;
    int noDataCnt = 0;
    while(s_blStreamIsDumping)
    {
        memset(&stNalu, 0, sizeof(stNalu));
        ret = ringBufferPop(&stNalu);
        if (ret != 0)
        {
            if (ret == -1)
            {
                printf("=================ring buffer no data: noDataCnt = %d==================\n", noDataCnt++);
            }
            if (ret == -2)
            {
                //printf("=================No complete Nalu found in ring buffer==================\n");
            }

            if (getPullStat())
            {
                printf("=========================pull complete=========================\n");
                printf("========================recording end=====total frame = %d=====dropFrameNum=%d=============\n", s_s32FrameCnt,dropFrameNum);
                mp4ContextFinish();
                s_blStreamIsDumping = 0;
            }
            sleep(1);

            continue;
        }

        if (s32SpsPpsFlag != 0x11)
        {
            if (stNalu.type == 0x07) //sps
            {
                memset(s_stExtrPayload.spsData, 0, sizeof(s_stExtrPayload.spsData));
                memcpy(s_stExtrPayload.spsData, &stNalu.frameData[4], stNalu.size);
                s_stExtrPayload.spsLen = stNalu.size;
                s32SpsPpsFlag |= 0x10;
            }
            else if (stNalu.type == 0x08) //pps
            {
                memset(s_stExtrPayload.ppsData, 0, sizeof(s_stExtrPayload.ppsData));
                memcpy(s_stExtrPayload.ppsData, &stNalu.frameData[4], stNalu.size);
                s_stExtrPayload.ppsLen = stNalu.size;

                if (s32SpsPpsFlag == 0x10) //保证收到的sps和pps是连续的
                {
                    s32SpsPpsFlag |= 0x01;
                    ret = mp4ContextInit();
                    if (ret != 0)
                    {
                        printf("mp4ContextInit failed\n");
                        sleep(3);
                        s_blStreamIsDumping = false;
                    }
                    startTime = time(NULL);
                    printf("===========recording begin===========\n");
                }
            }
            else
            {
                printf("no consecutive sps and pps were received, drop......\n");
                dropFrameNum++;
                continue;
                ////直接丢弃
            }
        }
        switch(stNalu.type)
        {
            case 0x07: //sps
                printf("skip a sps\n");
                break;
            
            case 0x08: //pps
                printf("skip a sps\n");
                break;

            case 0x05: //I帧
                /*MP4 nalu 前四个字节表示NALU长度，且为大端存储*/
                printf("write a I frame\n");
                writeVideo(true, &stNalu);
                break;

            case 0x01: //非关键帧
                printf("write a p frame\n");
                writeVideo(false, &stNalu);
                break;

            default:
                printf("error, don't support nalu type %d\n", stNalu.type);
        }
        if (s_s32FrameCnt == WRITE_MAX_FRAME)
        {
            printf("========================recording end=====total frame = %d=====dropFrameNum=%d=============\n", s_s32FrameCnt,dropFrameNum);
            mp4ContextFinish();
            s_blStreamIsDumping = 0;
        }

    }
    printf("===========streamDumpProc thread exit===========\n");
}

int streamDumpInit(void)
{
    int ret = 0;
    pthread_t pid;
    s_blStreamIsDumping = true;
    ret = pthread_create(&pid, NULL, streamDumpProc, NULL);
    if (ret)
    {
        printf("pthread_create failed\n");
        s_blStreamIsDumping = false;
        return -1;
    }

    return 0;
}


void streamDumpUninit(void)
{
    s_blStreamIsDumping = false;
}