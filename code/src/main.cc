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

bool quit = false;

static void sigterm_handler(int sig) {
    fprintf(stderr, "Caught signal %d, cleaning up...\n", sig);
    quit = true;
}

// 主程序入口
int main(int argc, char *argv[]) 
{
    // 使用video.h中定义的宏来设置分辨率参数
    int width = IMAGE_WIDTH;
    int height = IMAGE_HEIGHT;
    int model_width = MODEL_WIDTH;
    int model_height = MODEL_HEIGHT;

    // 注册信号处理函数，捕获 SIGINT 和 SIGTERM
    //signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    // 初始化云台
    printf("开始云台初始化！\n");
    Servo* g_servo = new Servo();
    printf("云台初始化成功\n");

    // 初始化并启动视频处理模块（不依赖TCP，优先启动）
    printf("开始视频初始化！\n");
    Video* g_video = new Video(width, height, model_width, model_height);
    if (g_video->init()) {
        printf("视频模块初始化成功！\n");
        g_video->start();
        printf("视频线程已自动启动\n");
        
        // ✅ 启动ONVIF服务（端口8080）
        printf("开始启动ONVIF服务...\n");
        if (g_video->startOnvif(8080)) {
            printf("✅ ONVIF服务启动成功！\n");
            printf("   - 设备发现地址: http://设备IP:8080/onvif/device_service\n");
            printf("   - 客户端可通过ONVIF协议发现并获取RTSP流地址\n");
        } else {
            printf("❌ ONVIF服务启动失败（不影响RTSP推流）\n");
        }
    } else {
        printf("视频模块初始化失败！\n");
    }

    // 初始化TCP客户端（后台自动重连，不阻塞）
    printf("开始TCP初始化（后台自动重连）...\n");
    TcpClient* g_tcp = new TcpClient("192.168.1.201", 8890);
    g_tcp->start();  // 启动TCP线程（内部会自动重连）
    printf("TCP线程已启动（将在后台自动尝试连接）\n");

    // 实例化Control对象，传入所有需要的指针
    Control* g_control = new Control(g_servo, g_video, g_tcp);
    printf("Control模块已初始化\n");

    // 设置TCP客户端的Control指针，使其能够处理接收到的数据
    g_tcp->setControl(g_control);
    printf("TCP-Control关联已建立\n");

    // 设置Video的Control指针，使其能够通知Control检测结果
    g_video->setControl(g_control);
    printf("Video-Control关联已建立\n");

    char cmd[128];
    printf("\n===========================================\n");
    printf("系统就绪！支持本地命令输入（格式与TCP相同）\n");
    printf("命令示例：\n");
    printf("  DEVICE_1:OP_1:VALUE_90    - 云台控制\n");
    printf("  DEVICE_2:OP_6:VALUE_1     - 启动AI识别\n");
    printf("  DEVICE_2:OP_9:VALUE_0     - 暂停视频流\n");
    printf("  RECT:100,100,200,150      - 设置检测区域\n");
    printf("  LIST:0,1,2                - 设置检测对象\n");
    printf("  quit                      - 退出程序\n");
    printf("===========================================\n");
    
    while (!quit)
    {
        printf("> ");
        fflush(stdout);
        if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
        
        // 去除换行符
        cmd[strcspn(cmd, "\r\n")] = 0;
        
        if (strcmp(cmd, "quit") == 0) {
            quit = true;
        } else if (strlen(cmd) > 0) {
            // 处理本地命令（格式与TCP相同）
            printf("本地命令: %s\n", cmd);
            if (strncmp(cmd, "RECT:", 5) == 0) {
                g_control->parseRectInfo(std::string(cmd));
            } else if (strncmp(cmd, "LIST:", 5) == 0) {
                g_control->parseObjList(std::string(cmd));
            } else {
                g_control->parseAndDispatch(std::string(cmd));
            }
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
