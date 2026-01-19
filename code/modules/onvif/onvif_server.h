#ifndef ONVIF_SERVER_H
#define ONVIF_SERVER_H

#include <string>
#include <pthread.h>

// 前向声明
class Video;

/**
 * ONVIF服务器
 * 实现ONVIF Profile S核心功能：
 * 1. Device Management - 设备发现、信息获取
 * 2. Media Management - 流地址获取
 * 3. PTZ Control (可选) - 云台控制
 */
class OnvifServer {
public:
    OnvifServer(Video* video, int port = 8080);
    ~OnvifServer();
    
    // 启动/停止服务
    bool start();
    void stop();
    bool isRunning() const { return running_; }
    
    // 设置设备信息
    void setDeviceInfo(const std::string& manufacturer, 
                      const std::string& model,
                      const std::string& firmware,
                      const std::string& serial);
    
    // 设置RTSP流地址
    void setRtspUrl(const std::string& url);
    
    // 获取服务端口
    int getPort() const { return port_; }
    
private:
    // 服务线程入口
    static void* serverThreadFunc(void* arg);
    void runServer();
    
    // HTTP请求处理
    void handleHttpRequest(int client_socket);
    
    // ONVIF SOAP请求处理
    std::string handleGetDeviceInformation();
    std::string handleGetCapabilities();
    std::string handleGetProfiles();
    std::string handleGetStreamUri(const std::string& profile_token);
    std::string handleGetSystemDateAndTime();
    std::string handleGetServices();
    std::string handleGetVideoEncoderConfiguration();
    
    // 工具函数
    std::string parseSOAPAction(const std::string& request);
    std::string parseSOAPBody(const std::string& request);
    std::string buildSOAPResponse(const std::string& body);
    std::string getCurrentDateTime();
    std::string getLocalIP();
    
private:
    Video* video_;              // Video模块指针
    int port_;                  // 服务端口（默认8080）
    bool running_;              // 运行状态
    pthread_t thread_;          // 服务线程
    int server_socket_;         // 服务器socket
    
    // 设备信息
    std::string manufacturer_;   // 制造商
    std::string model_;          // 型号
    std::string firmware_version_; // 固件版本
    std::string serial_number_;  // 序列号
    std::string rtsp_url_;       // RTSP流地址
    std::string local_ip_;       // 本地IP
};

#endif // ONVIF_SERVER_H
