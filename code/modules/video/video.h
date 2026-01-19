// video.h - 视频处理模块头文件
// 四线程架构封装：采集、BGR转换、推理、编码推流
// 性能优化：三缓冲区零拷贝、跳帧推理、硬件编码
#ifndef VIDEO_H
#define VIDEO_H

// 性能计时开关：定义此宏以启用各线程处理时间统计（用于性能调优）
// #define ENABLE_PERFORMANCE_TIMING  // ❌ 已关闭性能计时（减少日志输出）

// 帧率统计开关：分别控制终端输出和画面显示
// #define ENABLE_FPS_CONSOLE  // ❌ 已关闭FPS终端日志输出
#define ENABLE_FPS_DISPLAY  // ❌ 已关闭FPS画面显示（右上角FPS和运行时间）


// 图像分辨率配置宏（用于采集和编码）
#define IMAGE_WIDTH  1024    // 采集图像宽度
#define IMAGE_HEIGHT 600     // 采集图像高度

// 模型输入分辨率配置宏（用于推理）
#define MODEL_WIDTH  640     // 模型输入宽度
#define MODEL_HEIGHT 640     // 模型输入高度

// 推理帧率配置宏（用于控制推理频率）
// 值为 N 表示每 (N+1) 帧推理一次
// 例如：2 表示每3帧推理1次（跳过2帧）
//      4 表示每5帧推理1次（跳过4帧）
#define INFERENCE_FRAME_SKIP 3

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "yolov5.h"
#include <vector>
#include "common.h"

// 前向声明
class Control;
class OnvifServer;  // ONVIF服务器前向声明

// Video类：封装视频采集、推理、编码、推流等功能，支持独立线程运行
class Video {
public:
    // 构造函数，设置分辨率和模型输入尺寸
    Video(int width, int height, int model_width, int model_height);
    // 析构函数，自动释放资源
    ~Video();

    // 初始化所有资源（模型、ISP、VI、VENC、RTSP等）
    bool init();
    // 启动主循环线程
    bool start();
    // 停止主循环线程并清理资源
    void stop();
    
    // ✅ 视频流控制（暂停/恢复所有线程）
    // 暂停所有线程（采集、BGR转换、推理、编码推流）
    void pauseAllThreads();
    // 恢复所有线程
    void resumeAllThreads();
    // 检查是否正在运行
    bool isRunning() const { return running_; }
    
    // 开启AI识别
    void startAI();
    // 关闭AI识别
    void stopAI();
    // 开启区域识别
    void startAreaDetect();
    // 关闭区域识别
    void stopAreaDetect();
    // 开启对象识别
    void startObjectDetect();
    // 关闭对象识别
    void stopObjectDetect();
    // 获取当前矩形框信息
    void getRectInfo(const RectInfo& info);
    // 获取对象列表
    void getObjectList(const std::vector<int>& objList);
    // 设置Control对象指针
    void setControl(Control* control);
    // 设置检测结果发送间隔（秒）
    void setSendInterval(int interval);
    
    // ✅ ONVIF协议支持
    // 启动ONVIF服务（端口默认8080）
    bool startOnvif(int port = 8080);
    // 停止ONVIF服务
    void stopOnvif();
    // 获取RTSP流地址（供ONVIF调用）
    std::string getRtspUrl() const;
         

private:
    // 检测结果信息结构
    struct DetectionInfo {
        int cls_id;
        std::string cls_name;
        int x, y, w, h;
        float confidence;
    };
    
    // 四个线程入口函数
    static void* captureThreadFunc(void* arg);
    static void* bgrConvertThreadFunc(void* arg);  // BGR转换线程入口
    static void* inferenceThreadFunc(void* arg);
    static void* encodeThreadFunc(void* arg);
    
    // 四个线程的主循环
    void captureLoop();      // 采集循环：VI获取YUV→快速拷贝(32ms)
    void bgrConvertLoop();   // BGR转换循环：YUV→BGR转换(37ms瓶颈)+指针轮换
    void inferenceLoop();    // 推理循环：BGR+letterbox+RKNN(182ms,每3帧)
    void encodeLoop();       // 编码推流循环：绘制+H.264+RTSP(15ms)
    
    // letterbox处理，适配模型输入（INTER_NEAREST优化）
    cv::Mat letterbox(cv::Mat input);
    // 坐标映射回原图
    void mapCoordinates(int *x, int *y);
    // 构建检测结果汇总字符串
    std::string buildDetectionSummary();

    // 图像和模型相关参数
    int width_;
    int height_;
    int model_width_;
    int model_height_;
    float scale_;
    int leftPadding_;
    int topPadding_;

    // rknn推理相关
    rknn_app_context_t rknn_app_ctx_;
    object_detect_result_list od_results_;
    char text_[16];

    // 视频采集/编码相关
    VENC_STREAM_S stFrame_;
    RK_U32 H264_TimeRef_;
    VIDEO_FRAME_INFO_S stViFrame_;
    MB_POOL src_Pool_;
    MB_BLK src_Blk_;
    VIDEO_FRAME_INFO_S h264_frame_;
    cv::Mat frame_;
    unsigned char* data_;

    // RTSP相关
    rtsp_demo_handle g_rtsplive_;
    rtsp_session_handle g_rtsp_session_;

    // 四线程相关
    pthread_t thread_capture_;       // 采集线程（VI硬件获取YUV）
    pthread_t thread_bgr_convert_;   // BGR转换线程（YUV→BGR，37ms瓶颈）
    pthread_t thread_inference_;     // 推理线程（RKNN AI检测，每3帧）
    pthread_t thread_encode_;        // 编码推流线程（H.264+RTSP）
    bool running_;                   // 所有线程运行标志（控制暂停/恢复）
    bool ai_enable_;                 // AI识别开关标志
    bool area_enable_;               // 区域识别开关标志
    bool obj_enable_;                // 对象识别开关标志
    
    // 线程间共享缓冲区（生产者-消费者模式，互斥锁保护）
    struct FrameBuffer {
        unsigned char* yuv_data;
        uint64_t timestamp;
        bool ready;
        pthread_mutex_t mutex;
    };
    FrameBuffer capture_buffer_;     // 采集->BGR转换（YUV数据）
    
    // BGR三缓冲区（零拷贝优化：write/ready/read指针轮换）
    // 原理：转换完成时交换指针而非拷贝数据（8字节vs2.7MB）
    struct BGRFrameBuffer {
        cv::Mat* bgr_write;          // 写缓冲（BGR转换线程独占）
        cv::Mat* bgr_ready;          // 就绪缓冲（等待编码线程消费）
        cv::Mat* bgr_read;           // 读缓冲（编码线程独占）
        uint64_t timestamp;          // 时间戳
        int frame_index;             // 帧序号（避免重复处理）
        bool ready;                  // 数据就绪标志
        pthread_mutex_t mutex;       // 互斥锁
    };
    BGRFrameBuffer bgr_buffer_;      // BGR转换线程输出 -> 编码线程输入
    
    // 推理专用BGR三缓冲区（零拷贝优化：write/ready/read指针轮换）
    // 原理：BGR转换线程浅拷贝到write，轮换指针给推理线程，避免18ms深拷贝
    struct InferenceBGRBuffer {
        cv::Mat* bgr_write;          // 写缓冲（BGR转换线程独占，浅拷贝）
        cv::Mat* bgr_ready;          // 就绪缓冲（等待推理线程消费）
        cv::Mat* bgr_read;           // 读缓冲（推理线程独占）
        int frame_index;             // 帧序号（配合跳帧逻辑）
        bool ready;                  // 数据就绪标志
        pthread_mutex_t mutex;       // 互斥锁
    };
    InferenceBGRBuffer inference_bgr_buffer_;  // BGR转换线程输出 -> 推理线程输入
    
    // 共享检测结果（推理->编码，互斥锁保护）
    std::vector<DetectionInfo> shared_detections_;
    pthread_mutex_t detection_mutex_;
    
    // 检测结果缓存（编码线程本地缓存，避免每帧都卡顿）
    std::vector<DetectionInfo> cached_detections_;  // 上次推理结果缓存
    bool has_detection_result_;                      // 是否有过推理结果
    
    int inference_frame_skip_;       // 推理跳帧计数（每N+1帧推理1次，当前N=2）

    // 矩形框信息
    RectInfo video_rectInfo;
    // 对象列表
    std::vector<int> video_objList;
    // Control对象指针
    Control* control_;
    
    // 时间控制相关（限制发送频率）
    time_t last_send_time_;        // 上次发送时间
    int send_interval_;            // 发送间隔（秒），可动态调整
    static const int DEFAULT_SEND_INTERVAL = 1; // 默认发送间隔（秒）
    
    std::vector<DetectionInfo> current_detections_;
    
    // 帧率计算相关（使用高精度时间，微秒级）
#if defined(ENABLE_FPS_CONSOLE) || defined(ENABLE_FPS_DISPLAY)
    int frame_count_;              // 帧计数器
    struct timeval fps_start_time_;  // FPS计算开始时间（微秒精度）
    float current_fps_;            // 当前FPS值（编码线程统计）
#endif
#ifdef ENABLE_FPS_DISPLAY
    struct timeval program_start_time_; // 程序启动时间（用于运行时间显示）
#endif
    
    // ✅ ONVIF服务器对象
    OnvifServer* onvif_server_;    // ONVIF服务器指针
};

#endif // VIDEO_H
