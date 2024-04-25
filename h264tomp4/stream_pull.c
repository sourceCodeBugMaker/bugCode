#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>  
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include "ringBuffer.h"
#include "stream_pull.h"
#include "stream_dump.h"

#define SERVER_IP "192.168.5.88"
#define SERVER_PORT 10086
static int s_s32SockFd = -1;
static bool s_blStreamIsPulling = false;
#define BUFFER_SIZE (64 * 1024)


static int pull_complete = 0;

int getPullStat(void)
{
    return pull_complete;
}

static int udpSocketInit(void)
{
    int ret = 0;
    int optVal;
    socklen_t optLen;
    struct sockaddr_in server_addr;
	
    //创建udp socket
	s_s32SockFd = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_s32SockFd < 0) 
    {  
		printf("socket creation failed\n");  
		return -1;
	}  

	// 设置服务器地址信息  
	memset(&server_addr, 0, sizeof(server_addr));  
	server_addr.sin_family = AF_INET;  
	server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);  
	server_addr.sin_port = htons(SERVER_PORT);  

    //获取设置后的缓冲区大小
    optLen = sizeof(optVal);
    ret = getsockopt(s_s32SockFd, SOL_SOCKET, SO_RCVBUF, &optVal, &optLen);
    if (ret < 0)
    {
        printf("getsockopt failed");
        close(s_s32SockFd);
        s_s32SockFd = -1;
        return -1;
    }
    printf("current receive buffer size: %d bytes\n", optVal); 

    if (optLen < BUFFER_SIZE)
    {
        //设置socket接收缓冲区大小
        optVal = BUFFER_SIZE; 
        ret = setsockopt(s_s32SockFd, SOL_SOCKET, SO_RCVBUF, &optVal, sizeof(optVal));
        if (ret < 0) 
        {  
            printf("setsockopt failed");
            close(s_s32SockFd);
            s_s32SockFd = -1;
            return -1;
        }

        //获取设置后的缓冲区大小
        optLen = sizeof(optVal);
        ret = getsockopt(s_s32SockFd, SOL_SOCKET, SO_RCVBUF, &optVal, &optLen);
        if (ret < 0)
        {
            printf("getsockopt failed");
            close(s_s32SockFd);
            s_s32SockFd = -1;
            return -1;
        }
        printf("actual receive buffer size: %d bytes\n", optVal); 
    }


	// 绑定socket到服务器地址  
	ret = bind(s_s32SockFd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0)
    {  
		printf("bind failed\n"); 
        close(s_s32SockFd);
        s_s32SockFd = -1; 
		return -1;
	}

    return 0;
}


static void *streamPullProc(void *argc)
{
    prctl(PR_SET_NAME, "streamPullProc");
    if (pthread_detach(pthread_self()))
    {
        printf("pthrad detach error\n");
        return NULL;
    }

    FILE *file;
    size_t bytes_read;

    file = fopen("raw_stream.h264", "rb");
    if (file == NULL) {
        perror("Error opening file");
        return NULL;
    }

    char buffer[BUFFER_SIZE];
    int recvLen;
    int total_read = 0;
    int rewriteCnt = 0;
    pull_complete = 0;

    while (s_blStreamIsPulling)
    { 
		// memset(buffer, 0, BUFFER_SIZE);
		// recvLen = recvfrom(s_s32SockFd, buffer, BUFFER_SIZE, 0, NULL, NULL);
		// if (recvLen > 0)
		// {
        //     printf("----------revc size = %d----------\n", recvLen);
        //     while(0 != ringBufferPush(buffer, recvLen))
        //     {
        //         rewriteCnt++;
        //         printf("----------push again after 1ms: size = %d----------\n", recvLen);
        //         usleep(1000);
        //     }
		// }

        memset(buffer, 0, BUFFER_SIZE);

        if ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            while(ringBufferPush(buffer, bytes_read) != 0)
            {
                printf("===============write fail, again=========================\n");
                sleep(2);
                rewriteCnt++;
            }
            total_read += bytes_read;
        }
        else
        {
            pull_complete = 1;
            printf("===============read end  total = %d=========================\n", total_read);
            sleep(10);
        }

    }

    close(s_s32SockFd);
    s_s32SockFd = -1;
}

int streamPullInit(void)
{
    int ret = 0;
    pthread_t pid;

    ret = udpSocketInit();
    if (ret != 0)
    {
        printf("udpSocketInit failed\n");
        return -1;
    }

    s_blStreamIsPulling = true;
    ret = pthread_create(&pid, NULL, streamPullProc, NULL);
    if (ret)
    {
        close(s_s32SockFd);
        s_s32SockFd = -1;
        s_blStreamIsPulling = false;
        printf("pthread_create failed\n");
        return -1;
    }

    return 0;
}

void streamPullUninit(void)
{
    s_blStreamIsPulling = false;
}