// main.cc 详细中文注释版
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include "tcp.h"

#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "servo.h"
#include "control.h"
#include "video.h"

#define DISP_WIDTH  720   // 显示宽度
#define DISP_HEIGHT 480   // 显示高度

Servo g_servo;
Video g_video(720, 480, 640, 640);
Control g_control(&g_servo, &g_video);

bool quit = false;

static void sigterm_handler(int sig) {
    fprintf(stderr, "Caught signal %d, cleaning up...\n", sig);
    quit = true;
}

// 主程序入口
int main(int argc, char *argv[]) 
{
    int width = DISP_WIDTH;
    int height = DISP_HEIGHT;
    int model_width = 640;
    int model_height = 640;

    // 注册信号处理函数，捕获 SIGINT 和 SIGTERM
    //signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

	printf("开始tcp初始化！\n");

	// 初始化并启动TCP客户端
    TcpClient tcp("192.168.1.155", 8890);
    if (tcp.init()) {
        printf("TCP客户端初始化成功！\n");
    } else {
        printf("TCP客户端初始化失败！\n");
    }

	// 启动TCP线程
    if (tcp.isConnected()) {
        tcp.start();
        printf("TCP线程已自动启动\n");
    } else {
        printf("TCP未连接，无法自动启动线程！\n");
    }

    // 初始化并启动视频处理模块
    if (g_video.init()) {
        printf("视频模块初始化成功！\n");
        g_video.start();
        printf("视频线程已自动启动\n");
    } else {
        printf("视频模块初始化失败！\n");
    }

    char cmd[64];
    printf("请输入命令（quit）：\n");
    while (!quit)
    {
        printf("> ");
        fflush(stdout);
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
        // 去除换行符
        cmd[strcspn(cmd, "\r\n")] = 0;
        if (strcmp(cmd, "quit") == 0) {
            quit = true;
        } else {
            printf("未知命令: %s\n", cmd);
        }
    }
	// 等待视频线程和TCP线程结束
    printf("正在停止视频线程...\n");
    g_video.stop();
    printf("视频线程已停止\n");

    printf("正在停止TCP线程...\n");
    tcp.stop();
    printf("TCP线程已停止\n");

    printf("程序已安全退出\n");
    return 0;
}
