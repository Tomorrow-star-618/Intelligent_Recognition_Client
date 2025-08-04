// video.cc - 视频处理模块实现，采集、推理、编码、推流等功能，支持线程化
#include "video.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// 构造函数，初始化参数
Video::Video(int width, int height, int model_width, int model_height)
    : width_(width), height_(height), model_width_(model_width), model_height_(model_height),
      H264_TimeRef_(0), g_rtsplive_(NULL), g_rtsp_session_(NULL), running_(false) {
    memset(&rknn_app_ctx_, 0, sizeof(rknn_app_context_t));
}

// 析构函数，自动释放资源
Video::~Video() {
    stop();
}

// 初始化所有资源（模型、ISP、VI、VENC、RTSP等）
bool Video::init() {
    system("RkLunch-stop.sh"); // 停止可能占用资源的进程
    const char *model_path = "./model/yolov5.rknn";
    if (init_yolov5_model(model_path, &rknn_app_ctx_) != 0) {
        printf("init rknn model failed!\n");
        return false;
    }
    printf("init rknn model success!\n");
    init_post_process();

    // 创建编码帧结构体和内存池
    stFrame_.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    MB_POOL_CONFIG_S PoolCfg;
    memset(&PoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    PoolCfg.u64MBSize = width_ * height_ * 3;
    PoolCfg.u32MBCnt = 1;
    PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    src_Pool_ = RK_MPI_MB_CreatePool(&PoolCfg);
    printf("Create Pool success!\n");
    src_Blk_ = RK_MPI_MB_GetMB(src_Pool_, width_ * height_ * 3, RK_TRUE);
    h264_frame_.stVFrame.u32Width = width_;
    h264_frame_.stVFrame.u32Height = height_;
    h264_frame_.stVFrame.u32VirWidth = width_;
    h264_frame_.stVFrame.u32VirHeight = height_;
    h264_frame_.stVFrame.enPixelFormat = RK_FMT_RGB888;
    h264_frame_.stVFrame.u32FrameFlag = 160;
    h264_frame_.stVFrame.pMbBlk = src_Blk_;
    data_ = (unsigned char *)RK_MPI_MB_Handle2VirAddr(src_Blk_);
    frame_ = cv::Mat(cv::Size(width_, height_), CV_8UC3, data_);

    // ISP初始化
    RK_BOOL multi_sensor = RK_FALSE;
    const char *iq_dir = "/etc/iqfiles";
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir);
    SAMPLE_COMM_ISP_Run(0);

    // 系统、RTSP、VI、VENC初始化
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("rk mpi sys init fail!");
        return false;
    }
    g_rtsplive_ = create_rtsp_demo(554);
    g_rtsp_session_ = rtsp_new_session(g_rtsplive_, "/live/0");
    rtsp_set_video(g_rtsp_session_, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(g_rtsp_session_, rtsp_get_reltime(), rtsp_get_ntptime());
    vi_dev_init();
    vi_chn_init(0, width_, height_);
    RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
    venc_init(0, width_, height_, enCodecType);
    printf("venc init success\n");
    return true;
}

// 启动主循环线程
bool Video::start() {
    running_ = true;
    return pthread_create(&thread_, NULL, threadFunc, this) == 0;
}

// 停止线程并释放所有资源
void Video::stop() {
    running_ = false;
    if (thread_) {
        pthread_join(thread_, NULL);
        thread_ = 0;
    }
    // 资源释放
    RK_MPI_MB_ReleaseMB(src_Blk_);
    RK_MPI_MB_DestroyPool(src_Pool_);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);
    SAMPLE_COMM_ISP_Stop(0);
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);
    if(stFrame_.pstPack) free(stFrame_.pstPack);
    if(g_rtsplive_) rtsp_del_demo(g_rtsplive_);
    RK_MPI_SYS_Exit();
    release_yolov5_model(&rknn_app_ctx_);
    deinit_post_process();
}

// 线程入口，调用mainLoop
void* Video::threadFunc(void* arg) {
    Video* self = static_cast<Video*>(arg);
    self->mainLoop();
    return nullptr;
}

// 主循环：采集、推理、编码、推流
void Video::mainLoop() {
    RK_S32 s32Ret;
    int sX, sY, eX, eY;
    while(running_) {
        h264_frame_.stVFrame.u32TimeRef = H264_TimeRef_++;
        h264_frame_.stVFrame.u64PTS = TEST_COMM_GetNowUs();
        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame_, -1);
        if(s32Ret == RK_SUCCESS) {
            void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame_.stVFrame.pMbBlk);
            // YUV转BGR，OpenCV处理
            cv::Mat yuv420sp(height_ + height_ / 2, width_, CV_8UC1, vi_data);
            cv::Mat bgr(height_, width_, CV_8UC3, data_);
            cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);
            cv::resize(bgr, frame_, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
            // letterbox处理，适配模型输入
            cv::Mat letterboxImage = letterbox(frame_);
            memcpy(rknn_app_ctx_.input_mems[0]->virt_addr, letterboxImage.data, model_width_ * model_height_ * 3);
            // 推理
            inference_yolov5_model(&rknn_app_ctx_, &od_results_);
            // 处理推理结果，画框和标签
            for(int i = 0; i < od_results_.count; i++) {
                if(od_results_.count >= 1) {
                    object_detect_result *det_result = &(od_results_.results[i]);
                    sX = (int)(det_result->box.left);
                    sY = (int)(det_result->box.top);
                    eX = (int)(det_result->box.right);
                    eY = (int)(det_result->box.bottom);
                    mapCoordinates(&sX, &sY);
                    mapCoordinates(&eX, &eY);
                    printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id), sX, sY, eX, eY, det_result->prop);
                    cv::rectangle(frame_, cv::Point(sX, sY), cv::Point(eX, eY), cv::Scalar(0,255,0), 3);
                    sprintf(text_, "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
                    cv::putText(frame_, text_, cv::Point(sX, sY - 8), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0,255,0), 2);
                }
            }
            // 编码并推流
            memcpy(data_, frame_.data, width_ * height_ * 3);
            RK_MPI_VENC_SendFrame(0, &h264_frame_, -1);
            s32Ret = RK_MPI_VENC_GetStream(0, &stFrame_, -1);
            if(s32Ret == RK_SUCCESS) {
                if(g_rtsplive_ && g_rtsp_session_) {
                    void *pData = RK_MPI_MB_Handle2VirAddr(stFrame_.pstPack->pMbBlk);
                    rtsp_tx_video(g_rtsp_session_, (uint8_t *)pData, stFrame_.pstPack->u32Len, stFrame_.pstPack->u64PTS);
                    rtsp_do_event(g_rtsplive_);
                }
            }
            RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame_);
            RK_MPI_VENC_ReleaseStream(0, &stFrame_);
            memset(text_, 0, 8);
        }
        usleep(1000); // 防止CPU占用过高
    }
}

// letterbox处理：缩放+填充，适配模型输入
cv::Mat Video::letterbox(cv::Mat input) {
    float scaleX = (float)model_width_ / (float)width_;
    float scaleY = (float)model_height_ / (float)height_;
    scale_ = scaleX < scaleY ? scaleX : scaleY;
    int inputWidth = (int)((float)width_ * scale_);
    int inputHeight = (int)((float)height_ * scale_);
    leftPadding_ = (model_width_ - inputWidth) / 2;
    topPadding_ = (model_height_ - inputHeight) / 2;
    cv::Mat inputScale;
    cv::resize(input, inputScale, cv::Size(inputWidth,inputHeight), 0, 0, cv::INTER_LINEAR);
    cv::Mat letterboxImage(model_width_, model_height_, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Rect roi(leftPadding_, topPadding_, inputWidth, inputHeight);
    inputScale.copyTo(letterboxImage(roi));
    return letterboxImage;
}

// 坐标映射回原图
void Video::mapCoordinates(int *x, int *y) {
    int mx = *x - leftPadding_;
    int my = *y - topPadding_;
    *x = (int)((float)mx / scale_);
    *y = (int)((float)my / scale_);
}
