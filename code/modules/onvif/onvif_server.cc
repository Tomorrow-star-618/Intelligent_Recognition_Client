#include "onvif_server.h"
#include "../video/video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <time.h>
#include <sstream>

OnvifServer::OnvifServer(Video* video, int port)
    : video_(video), 
      port_(port), 
      running_(false), 
      thread_(0),
      server_socket_(-1),
      manufacturer_("SmartCamera"),
      model_("RV1106-AI-Camera"),
      firmware_version_("1.0.0"),
      serial_number_("RV1106-20241216001"),
      rtsp_url_("rtsp://192.168.1.100:554/live/0") {
    
    local_ip_ = getLocalIP();
}

OnvifServer::~OnvifServer() {
    stop();
}

bool OnvifServer::start() {
    if (running_) {
        printf("[ONVIF] 服务已在运行\n");
        return false;
    }
    
    // 创建服务器socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        printf("[ONVIF] 创建socket失败\n");
        return false;
    }
    
    // 设置端口重用
    int reuse = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // 绑定端口
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);
    
    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[ONVIF] 绑定端口 %d 失败\n", port_);
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }
    
    // 监听
    if (listen(server_socket_, 10) < 0) {
        printf("[ONVIF] 监听失败\n");
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }
    
    // 启动服务线程
    running_ = true;
    if (pthread_create(&thread_, NULL, serverThreadFunc, this) != 0) {
        printf("[ONVIF] 创建服务线程失败\n");
        close(server_socket_);
        server_socket_ = -1;
        running_ = false;
        return false;
    }
    
    printf("[ONVIF] ✅ 服务启动成功\n");
    printf("[ONVIF]    监听端口: %d\n", port_);
    printf("[ONVIF]    本地IP: %s\n", local_ip_.c_str());
    printf("[ONVIF]    设备地址: http://%s:%d/onvif/device_service\n", local_ip_.c_str(), port_);
    printf("[ONVIF]    RTSP流: %s\n", rtsp_url_.c_str());
    
    return true;
}

void OnvifServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // 关闭服务器socket（触发accept返回）
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }
    
    // 等待线程退出
    if (thread_) {
        pthread_join(thread_, NULL);
        thread_ = 0;
    }
    
    printf("[ONVIF] 服务已停止\n");
}

void OnvifServer::setDeviceInfo(const std::string& manufacturer, 
                                const std::string& model,
                                const std::string& firmware,
                                const std::string& serial) {
    manufacturer_ = manufacturer;
    model_ = model;
    firmware_version_ = firmware;
    serial_number_ = serial;
}

void OnvifServer::setRtspUrl(const std::string& url) {
    rtsp_url_ = url;
}

void* OnvifServer::serverThreadFunc(void* arg) {
    OnvifServer* self = static_cast<OnvifServer*>(arg);
    self->runServer();
    return nullptr;
}

void OnvifServer::runServer() {
    printf("[ONVIF] 服务线程运行中，等待客户端连接...\n");
    
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // 接受客户端连接
        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_socket < 0) {
            if (running_) {
                printf("[ONVIF] 接受连接失败\n");
            }
            break;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("[ONVIF] 客户端连接: %s:%d\n", client_ip, ntohs(client_addr.sin_port));
        
        // 处理HTTP/SOAP请求
        handleHttpRequest(client_socket);
        
        close(client_socket);
    }
    
    printf("[ONVIF] 服务线程退出\n");
}

void OnvifServer::handleHttpRequest(int client_socket) {
    char buffer[8192];
    memset(buffer, 0, sizeof(buffer));
    
    // 读取HTTP请求
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        return;
    }
    
    std::string request(buffer);
    
    // 解析SOAP Action
    std::string soap_action = parseSOAPAction(request);
    std::string response;
    
    printf("[ONVIF] 收到请求: %s\n", soap_action.c_str());
    
    // 根据SOAP Action路由到对应的处理函数
    if (soap_action.find("GetDeviceInformation") != std::string::npos) {
        response = handleGetDeviceInformation();
    }
    else if (soap_action.find("GetCapabilities") != std::string::npos) {
        response = handleGetCapabilities();
    }
    else if (soap_action.find("GetProfiles") != std::string::npos) {
        response = handleGetProfiles();
    }
    else if (soap_action.find("GetStreamUri") != std::string::npos) {
        response = handleGetStreamUri("Profile_1");
    }
    else if (soap_action.find("GetSystemDateAndTime") != std::string::npos) {
        response = handleGetSystemDateAndTime();
    }
    else if (soap_action.find("GetServices") != std::string::npos) {
        response = handleGetServices();
    }
    else if (soap_action.find("GetVideoEncoderConfiguration") != std::string::npos) {
        response = handleGetVideoEncoderConfiguration();
    }
    else {
        // 未实现的接口，返回空响应
        response = buildSOAPResponse("<s:Fault><s:Code><s:Value>s:Receiver</s:Value></s:Code>"
                                    "<s:Reason><s:Text>Action Not Implemented</s:Text></s:Reason></s:Fault>");
    }
    
    // 构建HTTP响应
    std::ostringstream http_response;
    http_response << "HTTP/1.1 200 OK\r\n";
    http_response << "Content-Type: application/soap+xml; charset=utf-8\r\n";
    http_response << "Content-Length: " << response.length() << "\r\n";
    http_response << "Connection: close\r\n";
    http_response << "\r\n";
    http_response << response;
    
    std::string response_str = http_response.str();
    send(client_socket, response_str.c_str(), response_str.length(), 0);
}

std::string OnvifServer::handleGetDeviceInformation() {
    std::ostringstream body;
    body << "<tds:GetDeviceInformationResponse>";
    body << "<tds:Manufacturer>" << manufacturer_ << "</tds:Manufacturer>";
    body << "<tds:Model>" << model_ << "</tds:Model>";
    body << "<tds:FirmwareVersion>" << firmware_version_ << "</tds:FirmwareVersion>";
    body << "<tds:SerialNumber>" << serial_number_ << "</tds:SerialNumber>";
    body << "<tds:HardwareId>RV1106</tds:HardwareId>";
    body << "</tds:GetDeviceInformationResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetCapabilities() {
    std::ostringstream body;
    body << "<tds:GetCapabilitiesResponse>";
    body << "<tds:Capabilities>";
    
    // Device能力
    body << "<tt:Device>";
    body << "<tt:XAddr>http://" << local_ip_ << ":" << port_ << "/onvif/device_service</tt:XAddr>";
    body << "<tt:System>";
    body << "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tt:SupportedVersions>";
    body << "</tt:System>";
    body << "</tt:Device>";
    
    // Media能力
    body << "<tt:Media>";
    body << "<tt:XAddr>http://" << local_ip_ << ":" << port_ << "/onvif/media_service</tt:XAddr>";
    body << "<tt:StreamingCapabilities>";
    body << "<tt:RTPMulticast>false</tt:RTPMulticast>";
    body << "<tt:RTP_TCP>true</tt:RTP_TCP>";
    body << "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>";
    body << "</tt:StreamingCapabilities>";
    body << "</tt:Media>";
    
    // Analytics能力（AI检测）
    body << "<tt:Analytics>";
    body << "<tt:XAddr>http://" << local_ip_ << ":" << port_ << "/onvif/analytics_service</tt:XAddr>";
    body << "<tt:RuleSupport>true</tt:RuleSupport>";
    body << "</tt:Analytics>";
    
    body << "</tds:Capabilities>";
    body << "</tds:GetCapabilitiesResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetProfiles() {
    std::ostringstream body;
    body << "<trt:GetProfilesResponse>";
    
    // Profile 1: 主码流
    body << "<trt:Profiles token=\"Profile_1\" fixed=\"true\">";
    body << "<tt:Name>MainStream</tt:Name>";
    
    // 视频源配置
    body << "<tt:VideoSourceConfiguration token=\"VideoSource_1\">";
    body << "<tt:Name>VideoSource</tt:Name>";
    body << "<tt:SourceToken>VideoSource_1</tt:SourceToken>";
    body << "<tt:Bounds x=\"0\" y=\"0\" width=\"1024\" height=\"600\"/>";
    body << "</tt:VideoSourceConfiguration>";
    
    // 视频编码配置
    body << "<tt:VideoEncoderConfiguration token=\"VideoEncoder_1\">";
    body << "<tt:Name>H264Encoder</tt:Name>";
    body << "<tt:Encoding>H264</tt:Encoding>";
    body << "<tt:Resolution><tt:Width>1024</tt:Width><tt:Height>600</tt:Height></tt:Resolution>";
    body << "<tt:Quality>4</tt:Quality>";
    body << "<tt:RateControl>";
    body << "<tt:FrameRateLimit>30</tt:FrameRateLimit>";
    body << "<tt:EncodingInterval>1</tt:EncodingInterval>";
    body << "<tt:BitrateLimit>2048</tt:BitrateLimit>";
    body << "</tt:RateControl>";
    body << "<tt:H264><tt:GovLength>30</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>";
    body << "</tt:VideoEncoderConfiguration>";
    
    body << "</trt:Profiles>";
    body << "</trt:GetProfilesResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetStreamUri(const std::string& profile_token) {
    std::ostringstream body;
    body << "<trt:GetStreamUriResponse>";
    body << "<trt:MediaUri>";
    body << "<tt:Uri>" << rtsp_url_ << "</tt:Uri>";
    body << "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>";
    body << "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>";
    body << "<tt:Timeout>PT60S</tt:Timeout>";
    body << "</trt:MediaUri>";
    body << "</trt:GetStreamUriResponse>";
    
    printf("[ONVIF] 返回RTSP流地址: %s\n", rtsp_url_.c_str());
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetSystemDateAndTime() {
    std::ostringstream body;
    body << "<tds:GetSystemDateAndTimeResponse>";
    body << "<tds:SystemDateAndTime>";
    body << "<tt:DateTimeType>Manual</tt:DateTimeType>";
    body << "<tt:DaylightSavings>false</tt:DaylightSavings>";
    
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    
    body << "<tt:TimeZone><tt:TZ>CST-8</tt:TZ></tt:TimeZone>";
    body << "<tt:UTCDateTime>";
    body << "<tt:Time>";
    body << "<tt:Hour>" << tm_info->tm_hour << "</tt:Hour>";
    body << "<tt:Minute>" << tm_info->tm_min << "</tt:Minute>";
    body << "<tt:Second>" << tm_info->tm_sec << "</tt:Second>";
    body << "</tt:Time>";
    body << "<tt:Date>";
    body << "<tt:Year>" << (tm_info->tm_year + 1900) << "</tt:Year>";
    body << "<tt:Month>" << (tm_info->tm_mon + 1) << "</tt:Month>";
    body << "<tt:Day>" << tm_info->tm_mday << "</tt:Day>";
    body << "</tt:Date>";
    body << "</tt:UTCDateTime>";
    
    body << "</tds:SystemDateAndTime>";
    body << "</tds:GetSystemDateAndTimeResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetServices() {
    std::ostringstream body;
    body << "<tds:GetServicesResponse>";
    
    // Device Service
    body << "<tds:Service>";
    body << "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>";
    body << "<tds:XAddr>http://" << local_ip_ << ":" << port_ << "/onvif/device_service</tds:XAddr>";
    body << "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version>";
    body << "</tds:Service>";
    
    // Media Service
    body << "<tds:Service>";
    body << "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>";
    body << "<tds:XAddr>http://" << local_ip_ << ":" << port_ << "/onvif/media_service</tds:XAddr>";
    body << "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version>";
    body << "</tds:Service>";
    
    body << "</tds:GetServicesResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::handleGetVideoEncoderConfiguration() {
    std::ostringstream body;
    body << "<trt:GetVideoEncoderConfigurationResponse>";
    body << "<trt:Configuration token=\"VideoEncoder_1\">";
    body << "<tt:Name>H264Encoder</tt:Name>";
    body << "<tt:Encoding>H264</tt:Encoding>";
    body << "<tt:Resolution><tt:Width>1024</tt:Width><tt:Height>600</tt:Height></tt:Resolution>";
    body << "<tt:Quality>4</tt:Quality>";
    body << "<tt:RateControl>";
    body << "<tt:FrameRateLimit>30</tt:FrameRateLimit>";
    body << "<tt:BitrateLimit>2048</tt:BitrateLimit>";
    body << "</tt:RateControl>";
    body << "</trt:Configuration>";
    body << "</trt:GetVideoEncoderConfigurationResponse>";
    
    return buildSOAPResponse(body.str());
}

std::string OnvifServer::parseSOAPAction(const std::string& request) {
    size_t pos = request.find("SOAPAction:");
    if (pos == std::string::npos) {
        pos = request.find("Action:");
    }
    
    if (pos != std::string::npos) {
        size_t start = request.find("\"", pos);
        size_t end = request.find("\"", start + 1);
        if (start != std::string::npos && end != std::string::npos) {
            return request.substr(start + 1, end - start - 1);
        }
    }
    
    // 尝试从Body中解析
    if (request.find("GetDeviceInformation") != std::string::npos) return "GetDeviceInformation";
    if (request.find("GetCapabilities") != std::string::npos) return "GetCapabilities";
    if (request.find("GetProfiles") != std::string::npos) return "GetProfiles";
    if (request.find("GetStreamUri") != std::string::npos) return "GetStreamUri";
    if (request.find("GetSystemDateAndTime") != std::string::npos) return "GetSystemDateAndTime";
    if (request.find("GetServices") != std::string::npos) return "GetServices";
    if (request.find("GetVideoEncoderConfiguration") != std::string::npos) return "GetVideoEncoderConfiguration";
    
    return "Unknown";
}

std::string OnvifServer::buildSOAPResponse(const std::string& body) {
    std::ostringstream response;
    response << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    response << "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" ";
    response << "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" ";
    response << "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" ";
    response << "xmlns:tt=\"http://www.onvif.org/ver10/schema\">";
    response << "<s:Body>";
    response << body;
    response << "</s:Body>";
    response << "</s:Envelope>";
    
    return response.str();
}

std::string OnvifServer::getLocalIP() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return "192.168.1.100";
    }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    
    if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
        // 尝试wlan0
        strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
            close(sock);
            return "192.168.1.100";
        }
    }
    
    close(sock);
    
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    return std::string(inet_ntoa(addr->sin_addr));
}
