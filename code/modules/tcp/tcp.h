// tcp.h - TCP客户端模块头文件，支持线程化心跳和接收
#ifndef TCP_H
#define TCP_H

#include <string>
#include <pthread.h>

// 前向声明
class Control;

// TcpClient类：封装TCP客户端功能，支持独立线程心跳和接收
class TcpClient {
public:
    // 构造函数，指定远程IP和端口
    TcpClient(const std::string& ip, int port);
    // 析构函数，自动关闭连接
    ~TcpClient();
    // 初始化连接（只建立socket连接，不启动线程）
    bool init();
    // 启动主循环线程
    bool start();
    // 停止主循环线程并关闭socket
    void stop();
    // 查询当前连接状态
    bool isConnected() const;
    // 设置Control对象指针
    void setControl(Control* control);
    // 发送数据到服务器
    bool sendData(const std::string& data);
    
    // 动态更新服务器地址（用于UDP发现后更新）
    // 如果当前已连接到其他服务器，会先断开再重连新地址
    void updateServerAddress(const std::string& ip, int port = -1);
    
    // 获取当前服务器IP
    std::string getServerIp() const { return ip_; }

private:
    // 线程入口函数
    static void* threadFunc(void* arg);
    // 主循环，定时心跳和接收
    void run();

    std::string ip_;      // 远程服务器IP
    int port_;            // 远程服务器端口
    int sockfd_;          // socket文件描述符
    pthread_t thread_;    // 线程句柄
    bool running_;        // 线程运行标志
    bool connected_;      // 连接状态
    Control* control_;    // Control对象指针
    
    pthread_mutex_t mutex_;     // 互斥锁，保护IP更新
    bool addressUpdated_;       // 地址是否已更新标志
};

#endif // TCP_H
