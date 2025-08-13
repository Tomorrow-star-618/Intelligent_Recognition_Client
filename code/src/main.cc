// main.cc 详细中文注释版
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "control.h"
#include "servo.h"
#include "video.h"
#include "tcp.h"

#define DISP_WIDTH  720   // 显示宽度
#define DISP_HEIGHT 480   // 显示高度

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

    // 初始化云台
    printf("开始云台初始化！\n");
    Servo* g_servo = new Servo();
    printf("云台初始化成功\n");

	// 初始化并启动TCP客户端
    printf("开始tcp初始化！\n");
    TcpClient* g_tcp = new TcpClient("192.168.1.156", 8890);
    if (g_tcp->init()) {
        printf("TCP客户端初始化成功！\n");
    } else {
        printf("TCP客户端初始化失败！\n");
    }

	// 启动TCP线程（由start内部自动重连并启动）
    g_tcp->start();
    printf("TCP线程已自动启动\n");

    // 初始化并启动视频处理模块
    printf("开始视频初始化！\n");
    Video* g_video = new Video(width, height, model_width, model_height);
    if (g_video->init()) {
        printf("视频模块初始化成功！\n");
        g_video->start();
        printf("视频线程已自动启动\n");
    } else {
        printf("视频模块初始化失败！\n");
    }

    // 实例化Control对象，传入所有需要的指针
    Control* g_control = new Control(g_servo, g_video, g_tcp);
    printf("Control模块已初始化\n");

    // 设置TCP客户端的Control指针，使其能够处理接收到的数据
    g_tcp->setControl(g_control);
    printf("TCP-Control关联已建立\n");

    // 设置Video的Control指针，使其能够通知Control检测结果
    g_video->setControl(g_control);
    printf("Video-Control关联已建立\n");

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
    g_video->stop();
    printf("视频线程已停止\n");

    printf("正在停止TCP线程...\n");
    g_tcp->stop();
    printf("TCP线程已停止\n");

    // 清理Control对象
    if (g_control) {
        delete g_control;
        g_control = nullptr;
        printf("Control模块已清理\n");
    }

    // 清理其他动态分配的对象
    if (g_tcp) {
        delete g_tcp;
        g_tcp = nullptr;
        printf("TCP模块已清理\n");
    }

    if (g_video) {
        delete g_video;
        g_video = nullptr;
        printf("Video模块已清理\n");
    }

    if (g_servo) {
        delete g_servo;
        g_servo = nullptr;
        printf("Servo模块已清理\n");
    }

    printf("程序已安全退出\n");
    return 0;
}
