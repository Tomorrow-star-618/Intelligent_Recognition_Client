// tcp.h - TCP客户端模块头文件，支持线程化心跳和接收
#ifndef TCP_H
#define TCP_H

#include <string>
#include <pthread.h>

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
};

#endif // TCP_H
