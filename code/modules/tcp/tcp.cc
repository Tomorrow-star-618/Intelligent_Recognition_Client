// tcp.cc - TCP客户端实现，独立线程心跳与接收，适合嵌入式/实时场景
#include "tcp.h"
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// 构造函数，初始化成员变量
TcpClient::TcpClient(const std::string& ip, int port)
    : ip_(ip), port_(port), sockfd_(-1), running_(false), connected_(false), 
      control_(nullptr), addressUpdated_(false) 
{
    pthread_mutex_init(&mutex_, NULL);
}

// 析构函数，自动停止线程和关闭socket
TcpClient::~TcpClient() {
    stop();
    pthread_mutex_destroy(&mutex_);
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

// 启动TCP线程，线程内循环心跳和接收（支持自动重连）
bool TcpClient::start() {
    running_ = true;
    // 不阻塞主流程，线程内部会自动尝试连接
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

// 设置Control对象指针
void TcpClient::setControl(Control* control) {
    control_ = control;
}

// 发送数据到服务器
bool TcpClient::sendData(const std::string& data) {
    if (!connected_ || sockfd_ < 0) {
        printf("TCP未连接，无法发送数据: %s\n", data.c_str());
        return false;
    }
    
    ssize_t sent = send(sockfd_, data.c_str(), data.length(), 0);
    if (sent < 0) {
        perror("TCP发送数据失败");
        return false;
    } else if ((size_t)sent != data.length()) {
        printf("TCP数据发送不完整: 期望%zu字节，实际发送%zd字节\n", data.length(), sent);
        return false;
    }
    
    printf("TCP发送数据成功: %s\n", data.c_str());
    return true;
}

// 动态更新服务器地址
void TcpClient::updateServerAddress(const std::string& ip, int port) {
    pthread_mutex_lock(&mutex_);
    
    // 检查是否真正有变化
    bool changed = (ip_ != ip);
    if (port > 0 && port != port_) {
        changed = true;
    }
    
    if (!changed) {
        pthread_mutex_unlock(&mutex_);
        return;
    }
    
    printf("🔄 更新TCP服务器地址: %s:%d -> %s:%d\n", 
           ip_.c_str(), port_, ip.c_str(), port > 0 ? port : port_);
    
    ip_ = ip;
    if (port > 0) {
        port_ = port;
    }
    addressUpdated_ = true;
    
    pthread_mutex_unlock(&mutex_);
    
    // 如果当前已连接，断开连接以便重连到新地址
    if (connected_ && sockfd_ >= 0) {
        printf("🔌 断开当前TCP连接，准备连接新地址...\n");
        close(sockfd_);
        sockfd_ = -1;
        connected_ = false;
    }
}

// 线程入口函数，调用run()
void* TcpClient::threadFunc(void* arg) {
    TcpClient* self = static_cast<TcpClient*>(arg);
    self->run();
    return nullptr;
}

// 线程主循环：自动重连 + 接收服务器数据
void TcpClient::run() {
    char buf[128];
    
    while (running_) {
        // 检查是否有地址更新
        pthread_mutex_lock(&mutex_);
        bool addrUpdated = addressUpdated_;
        addressUpdated_ = false;
        std::string currentIp = ip_;
        int currentPort = port_;
        pthread_mutex_unlock(&mutex_);
        
        // 如果地址已更新且当前已连接，断开重连
        if (addrUpdated && connected_) {
            printf("🔄 地址已更新，断开当前连接...\n");
            if (sockfd_ >= 0) {
                close(sockfd_);
                sockfd_ = -1;
            }
            connected_ = false;
        }
        
        // 如果未连接，尝试连接
        if (!connected_) {
            // 检查IP是否有效
            if (currentIp.empty() || currentIp == "" || currentIp == "设备IP") {
                printf("⚠️  等待上位机发现（当前IP无效）...\n");
                sleep(3);
                continue;
            }
            
            printf("TCP尝试连接到 %s:%d...\n", currentIp.c_str(), currentPort);
            if (init()) {
                printf("✅ TCP连接成功！\n");
            } else {
                printf("❌ TCP连接失败，5秒后重试...\n");
                sleep(5);
                continue;
            }
        }
        
        // 接收数据
        ssize_t n = recv(sockfd_, buf, sizeof(buf)-1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';
            printf("TCP recv: %s\n", buf);
            // 如果Control对象存在，则处理接收到的数据
            if (control_) {
                // 判断前缀，选择不同解析函数
                if (strncmp(buf, "RECT:", 5) == 0) {
                    control_->parseRectInfo(std::string(buf));
                } else if (strncmp(buf, "LIST:", 5) == 0) {
                    control_->parseObjList(std::string(buf));
                } else {
                    control_->parseAndDispatch(std::string(buf));
                }
            } else {
                printf("Warning: Control对象未设置，无法处理接收到的数据\n");
            }
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            printf("TCP连接断开，5秒后自动重连...\n");
            connected_ = false;
            if (sockfd_ >= 0) {
                close(sockfd_);
                sockfd_ = -1;
            }
            sleep(5);
            continue;
        }
        usleep(10000);
    }
    
    printf("TCP线程退出\n");
}
