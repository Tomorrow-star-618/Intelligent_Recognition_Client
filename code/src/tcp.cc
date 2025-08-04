// tcp.cc - TCP客户端实现，独立线程心跳与接收，适合嵌入式/实时场景
#include "tcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// 构造函数，初始化成员变量
TcpClient::TcpClient(const std::string& ip, int port)
    : ip_(ip), port_(port), sockfd_(-1), running_(false), connected_(false) {}

// 析构函数，自动停止线程和关闭socket
TcpClient::~TcpClient() {
    stop();
}

// 初始化TCP连接（只建立连接，不启动线程）
bool TcpClient::init() {
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0); // 创建socket
    if (sockfd_ < 0) {
        perror("socket");
        return false;
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port_); // 设置端口
    if (inet_pton(AF_INET, ip_.c_str(), &serv_addr.sin_addr) <= 0) { // 设置IP
        perror("inet_pton");
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    if (connect(sockfd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { // 连接服务器
        perror("connect");
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    connected_ = true;
    printf("TCP client connected to %s:%d\n", ip_.c_str(), port_);
    return true;
}

// 启动TCP线程，线程内循环心跳和接收
bool TcpClient::start() {
    running_ = true;
    return pthread_create(&thread_, NULL, threadFunc, this) == 0;
}

// 停止线程并关闭socket
void TcpClient::stop() {
    running_ = false;
    if (thread_) {
        pthread_join(thread_, NULL); // 等待线程结束
        thread_ = 0;
    }
    if (sockfd_ >= 0) {
        close(sockfd_); // 关闭socket
        sockfd_ = -1;
    }
    connected_ = false;
}

// 查询当前连接状态
bool TcpClient::isConnected() const {
    return connected_;
}

// 线程入口函数，调用run()
void* TcpClient::threadFunc(void* arg) {
    TcpClient* self = static_cast<TcpClient*>(arg);
    self->run();
    return nullptr;
}

// 线程主循环：定时发送心跳，接收服务器数据
void TcpClient::run() {
    char buf[128];
    while (running_ && connected_) {
        // 每隔2秒向服务器发送心跳包
        const char* msg = "heartbeat";
        ssize_t sent = send(sockfd_, msg, strlen(msg), 0);
        if (sent <= 0) {
            perror("send");
            connected_ = false;
            break;
        }
        // 非阻塞接收服务器响应
        ssize_t n = recv(sockfd_, buf, sizeof(buf)-1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';
            printf("TCP recv: %s\n", buf);
        }
        sleep(2); // 心跳间隔
    }
}
