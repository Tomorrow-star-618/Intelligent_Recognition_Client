/**
 * @file video.h
 * @brief 视频处理模块 - 四线程异步架构
 * 
 * 架构：采集(30fps) → BGR转换(27fps) → [推理(6fps) + 编码推流(27fps)]
 * 优化：三缓冲零拷贝 + 智能跳帧 + 硬件加速(RGA/VENC/NPU)
 */
#ifndef VIDEO_H
#define VIDEO_H

// ==================== 性能调试开关 ====================
 #define ENABLE_PERFORMANCE_TIMING  // 性能计时（各线程耗时统计）
// #define ENABLE_FPS_CONSOLE         // FPS终端输出
#define ENABLE_FPS_DISPLAY            // FPS画面显示（右上角）

// ==================== 系统配置参数 ====================
#define IMAGE_WIDTH           1280    // 视频分辨率：宽度
#define IMAGE_HEIGHT          720     // 视频分辨率：高度
#define MODEL_WIDTH           640     // YOLO模型输入：宽度
#define MODEL_HEIGHT          640     // YOLO模型输入：高度
#define INFERENCE_FRAME_SKIP  3       // 推理跳帧：每4帧推理1次（0=每帧推理）

// ==================== 系统头文件 ====================
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <vector>

// ==================== 第三方库 ====================
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

// ==================== 项目头文件 ====================
#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "yolov5.h"
#include "common.h"

// ==================== 前向声明 ====================
class Control;
class OnvifServer;

// ==================== 前向声明 ====================
class Control;
class OnvifServer;

/**
 * @class Video
 * @brief 视频处理核心类
 * 
 * 功能模块：
 * 1. 视频采集（VI硬件）
 * 2. 图像转换（YUV→BGR，RGA加速）
 * 3. AI推理（YOLOv5，RKNN NPU加速）
 * 4. 视频编码（H.264，MPP VENC硬件加速）
 * 5. 流媒体推流（RTSP）
 * 6. 设备管理（ONVIF协议）
 */
class Video {
public:
    // ==================== 生命周期管理 ====================
    Video(int width, int height, int model_width, int model_height);
    ~Video();

    bool init();   // 初始化硬件资源（ISP/VI/VENC/RTSP/RKNN）
    bool start();  // 启动四线程（采集/转换/推理/编码）
    void stop();   // 停止所有线程并释放资源
    
    // ==================== 视频流控制 ====================
    void pauseAllThreads();   // 暂停所有线程（节省CPU）
    void resumeAllThreads();  // 恢复所有线程
    bool isRunning() const { return running_; }
    
    // ==================== AI功能控制 ====================
    void startAI();               // 启用AI识别（80类COCO物体）
    void stopAI();                // 禁用AI识别
    void startAreaDetect();       // 启用区域检测（ROI过滤）
    void stopAreaDetect();        // 禁用区域检测
    void startObjectDetect();     // 启用对象过滤（白名单）
    void stopObjectDetect();      // 禁用对象过滤
    void getRectInfo(const RectInfo& info);           // 设置检测区域
    void getObjectList(const std::vector<int>& list); // 设置检测对象白名单
    void setSendInterval(int interval);               // 设置检测结果上报间隔
    
    // ==================== 模块关联 ====================
    void setControl(Control* control);  // 设置控制模块指针（用于结果回调）
    
    // ==================== ONVIF协议 ====================
    bool startOnvif(int port = 8080);  // 启动ONVIF服务
    void stopOnvif();                  // 停止ONVIF服务
    std::string getRtspUrl() const;    // 获取RTSP流地址（动态IP）

private:
    // ==================== 数据结构 ====================
    
    /** 检测结果信息 */
    struct DetectionInfo {
        int cls_id;           // 类别ID（0-79）
        std::string cls_name; // 类别名称（person/car等）
        int x, y, w, h;       // 边界框（左上角x,y，宽w，高h）
        float confidence;     // 置信度（0.0-1.0）
    };
    
    /** YUV帧缓冲区（采集→BGR转换） */
    struct FrameBuffer {
        unsigned char* yuv_data;  // YUV数据指针
        uint64_t timestamp;       // 时间戳（微秒）
        bool ready;               // 数据就绪标志
        pthread_mutex_t mutex;    // 互斥锁
    };
    
    /** BGR三缓冲区（零拷贝优化：write/ready/read指针轮换） */
    struct BGRFrameBuffer {
        cv::Mat* bgr_write;   // 写缓冲（BGR转换线程独占）
        cv::Mat* bgr_ready;   // 就绪缓冲（等待消费）
        cv::Mat* bgr_read;    // 读缓冲（编码线程独占）
        uint64_t timestamp;   // 时间戳
        int frame_index;      // 帧序号（避免重复处理）
        bool ready;           // 数据就绪标志
        pthread_mutex_t mutex;
    };
    
    /** 推理专用BGR三缓冲区（零拷贝优化） */
    struct InferenceBGRBuffer {
        cv::Mat* bgr_write;   // 写缓冲（浅拷贝）
        cv::Mat* bgr_ready;   // 就绪缓冲
        cv::Mat* bgr_read;    // 读缓冲（推理线程独占）
        int frame_index;      // 帧序号
        bool ready;           // 数据就绪标志
        pthread_mutex_t mutex;
    };
    
    // ==================== 线程函数 ====================
    static void* captureThreadFunc(void* arg);     // 采集线程入口
    static void* bgrConvertThreadFunc(void* arg);  // BGR转换线程入口
    static void* inferenceThreadFunc(void* arg);   // 推理线程入口
    static void* encodeThreadFunc(void* arg);      // 编码线程入口
    
    void captureLoop();      // 采集主循环：VI获取YUV（30fps）
    void bgrConvertLoop();   // BGR转换主循环：YUV→BGR（27fps瓶颈）
    void inferenceLoop();    // 推理主循环：YOLO检测（6fps）
    void encodeLoop();       // 编码主循环：H.264+RTSP（27fps）
    
    // ==================== 图像处理 ====================
    cv::Mat letterbox(cv::Mat input);          // letterbox缩放（适配模型输入）
    void mapCoordinates(int* x, int* y);       // 坐标映射（模型→原图）
    std::string buildDetectionSummary();       // 构建检测结果字符串
    
    // ==================== 成员变量 ====================
    
    // --- 视频参数 ---
    int width_;           // 视频宽度
    int height_;          // 视频高度
    int model_width_;     // 模型输入宽度
    int model_height_;    // 模型输入高度
    float scale_;         // letterbox缩放比例
    int leftPadding_;     // letterbox左填充
    int topPadding_;      // letterbox上填充

    // --- RKNN推理 ---
    rknn_app_context_t rknn_app_ctx_;       // RKNN上下文
    object_detect_result_list od_results_;  // 检测结果列表
    char text_[16];                         // 临时文本缓冲

    // --- 视频采集/编码 ---
    VENC_STREAM_S stFrame_;           // 编码流结构
    RK_U32 H264_TimeRef_;             // H.264时间戳
    VIDEO_FRAME_INFO_S stViFrame_;    // VI帧信息
    MB_POOL src_Pool_;                // 内存池
    MB_BLK src_Blk_;                  // 内存块
    VIDEO_FRAME_INFO_S h264_frame_;   // H.264帧
    cv::Mat frame_;                   // OpenCV帧
    unsigned char* data_;             // 帧数据指针

    // --- RTSP推流 ---
    rtsp_demo_handle g_rtsplive_;     // RTSP服务句柄
    rtsp_session_handle g_rtsp_session_; // RTSP会话句柄

    // --- 线程管理 ---
    pthread_t thread_capture_;        // 采集线程
    pthread_t thread_bgr_convert_;    // BGR转换线程
    pthread_t thread_inference_;      // 推理线程
    pthread_t thread_encode_;         // 编码线程
    bool running_;                    // 运行标志（控制所有线程）
    
    // --- 功能开关 ---
    bool ai_enable_;                  // AI识别开关
    bool area_enable_;                // 区域检测开关
    bool obj_enable_;                 // 对象过滤开关
    
    // --- 线程间通信 ---
    FrameBuffer capture_buffer_;                  // 采集→BGR转换
    BGRFrameBuffer bgr_buffer_;                   // BGR转换→编码
    InferenceBGRBuffer inference_bgr_buffer_;     // BGR转换→推理
    std::vector<DetectionInfo> shared_detections_;   // 推理→编码（共享）
    std::vector<DetectionInfo> cached_detections_;   // 编码线程本地缓存
    pthread_mutex_t detection_mutex_;             // 检测结果互斥锁
    bool has_detection_result_;                   // 是否有检测结果
    
    // --- AI配置 ---
    int inference_frame_skip_;        // 推理跳帧计数
    RectInfo video_rectInfo;          // 检测区域信息
    std::vector<int> video_objList;   // 检测对象白名单
    
    // --- 模块关联 ---
    Control* control_;                // 控制模块指针
    time_t last_send_time_;           // 上次发送时间
    int send_interval_;               // 发送间隔（秒）
    static const int DEFAULT_SEND_INTERVAL = 1;
    std::vector<DetectionInfo> current_detections_;
    
    // --- FPS统计 ---
#if defined(ENABLE_FPS_CONSOLE) || defined(ENABLE_FPS_DISPLAY)
    int frame_count_;                 // 帧计数器
    struct timeval fps_start_time_;   // FPS计算起始时间
    float current_fps_;               // 当前FPS值
#endif
#ifdef ENABLE_FPS_DISPLAY
    struct timeval program_start_time_; // 程序启动时间
#endif
    
    // --- ONVIF服务 ---
    OnvifServer* onvif_server_;       // ONVIF服务器指针
};

#endif // VIDEO_H
