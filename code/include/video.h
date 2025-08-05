// video.h - 视频处理模块头文件，封装推理、采集、编码、推流等功能，支持线程化
#ifndef VIDEO_H
#define VIDEO_H

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <pthread.h>
#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "yolov5.h"

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
    // 开启AI识别
    void startAI();
    // 关闭AI识别
    void stopAI();

private:
    // 线程入口函数
    static void* threadFunc(void* arg);
    // 主循环，采集、推理、编码、推流
    void mainLoop();
    // letterbox处理，适配模型输入
    cv::Mat letterbox(cv::Mat input);
    // 坐标映射回原图
    void mapCoordinates(int *x, int *y);

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

    // 线程相关
    pthread_t thread_;
    bool running_;
    bool ai_enable_; // AI识别开关标志
};

#endif // VIDEO_H
