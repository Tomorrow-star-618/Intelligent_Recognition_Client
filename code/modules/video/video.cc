// video.cc - 视频处理模块实现，采集、推理、编码、推流等功能，支持线程化
#include "video.h"
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <iomanip>


// 构造函数，初始化参数
Video::Video(int width, int height, int model_width, int model_height)
    : width_(width), height_(height), model_width_(model_width), model_height_(model_height),
      H264_TimeRef_(0), g_rtsplive_(NULL), g_rtsp_session_(NULL), running_(false), 
      ai_enable_(false), area_enable_(false), obj_enable_(false), 
      control_(nullptr), last_send_time_(0), send_interval_(DEFAULT_SEND_INTERVAL),
      frame_count_(0), fps_start_time_(0), current_fps_(0.0f),
      thread_capture_(0), thread_inference_(0), thread_encode_(0),
      inference_frame_skip_(2) {  // 默认每3帧推理1次
    
    memset(&rknn_app_ctx_, 0, sizeof(rknn_app_context_t));
    
    // 初始化缓冲区
    int yuv_size = width_ * height_ * 3 / 2;
    capture_buffer_.yuv_data = (unsigned char*)malloc(yuv_size);
    capture_buffer_.ready = false;
    pthread_mutex_init(&capture_buffer_.mutex, NULL);
    
    inference_buffer_.yuv_data = (unsigned char*)malloc(yuv_size);
    inference_buffer_.ready = false;
    pthread_mutex_init(&inference_buffer_.mutex, NULL);
    
    pthread_mutex_init(&detection_mutex_, NULL);
}

// 析构函数，自动释放资源
Video::~Video() {
    stop();
    
    pthread_mutex_destroy(&capture_buffer_.mutex);
    pthread_mutex_destroy(&inference_buffer_.mutex);
    pthread_mutex_destroy(&detection_mutex_);
    
    if (capture_buffer_.yuv_data) {
        free(capture_buffer_.yuv_data);
        capture_buffer_.yuv_data = NULL;
    }
    if (inference_buffer_.yuv_data) {
        free(inference_buffer_.yuv_data);
        inference_buffer_.yuv_data = NULL;
    }
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

// 启动三线程
bool Video::start() {
    running_ = true;
    
    // 创建三个线程
    if (pthread_create(&thread_capture_, NULL, captureThreadFunc, this) != 0) {
        printf("[错误] 创建采集线程失败\n");
        return false;
    }
    
    if (pthread_create(&thread_inference_, NULL, inferenceThreadFunc, this) != 0) {
        printf("[错误] 创建推理线程失败\n");
        return false;
    }
    
    if (pthread_create(&thread_encode_, NULL, encodeThreadFunc, this) != 0) {
        printf("[错误] 创建编码线程失败\n");
        return false;
    }
    
    printf("[成功] 三线程启动: 采集、推理、编码推流\n");
    return true;
}

// 停止线程并释放所有资源
void Video::stop() {
    running_ = false;
    
    if (thread_capture_) {
        pthread_join(thread_capture_, NULL);
        thread_capture_ = 0;
        printf("[线程] 采集线程已停止\n");
    }
    if (thread_inference_) {
        pthread_join(thread_inference_, NULL);
        thread_inference_ = 0;
        printf("[线程] 推理线程已停止\n");
    }
    if (thread_encode_) {
        pthread_join(thread_encode_, NULL);
        thread_encode_ = 0;
        printf("[线程] 编码线程已停止\n");
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

// ============ 线程入口函数 ============
void* Video::captureThreadFunc(void* arg) {
    Video* self = static_cast<Video*>(arg);
    self->captureLoop();
    return nullptr;
}

void* Video::inferenceThreadFunc(void* arg) {
    Video* self = static_cast<Video*>(arg);
    self->inferenceLoop();
    return nullptr;
}

void* Video::encodeThreadFunc(void* arg) {
    Video* self = static_cast<Video*>(arg);
    self->encodeLoop();
    return nullptr;
}

// ============ 线程1: 采集循环 ============
void Video::captureLoop() {
    RK_S32 s32Ret;
    int frame_counter = 0;
    int yuv_size = width_ * height_ * 3 / 2;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end;
    long vi_time_us, memcpy_time_us;
#endif
    
    printf("[采集线程] 启动\n");
    
    while(running_) {
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame_, -1);
        if(s32Ret != RK_SUCCESS) {
            continue; // 移除usleep，立即重试
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        vi_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
#endif
        
        void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame_.stVFrame.pMbBlk);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        // 1. 拷贝到编码缓冲区（每帧都要）
        pthread_mutex_lock(&capture_buffer_.mutex);
        memcpy(capture_buffer_.yuv_data, vi_data, yuv_size);
        capture_buffer_.timestamp = TEST_COMM_GetNowUs();
        capture_buffer_.ready = true;
        pthread_mutex_unlock(&capture_buffer_.mutex);
        
        // 2. 按跳帧策略拷贝到推理缓冲区
        if (ai_enable_ && (frame_counter % (inference_frame_skip_ + 1)) == 0) {
            pthread_mutex_lock(&inference_buffer_.mutex);
            if (!inference_buffer_.ready) {  // 避免覆盖未处理的帧
                memcpy(inference_buffer_.yuv_data, vi_data, yuv_size);
                inference_buffer_.timestamp = capture_buffer_.timestamp;
                inference_buffer_.ready = true;
            }
            pthread_mutex_unlock(&inference_buffer_.mutex);
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        memcpy_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        
        // 每30帧打印一次
        if (frame_counter % 30 == 0) {
            printf("[采集] VI获取: %ld us, 内存拷贝: %ld us, 总计: %ld us\n", 
                   vi_time_us, memcpy_time_us, vi_time_us + memcpy_time_us);
        }
#endif
        
        RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame_);
        frame_counter++;
    }
    
    printf("[采集线程] 退出\n");
}

// ============ 线程2: 推理循环 ============
void Video::inferenceLoop() {
    cv::Mat yuv_mat, bgr_mat;
    int sX, sY, eX, eY;
    int yuv_size = width_ * height_ * 3 / 2;
    unsigned char* yuv_buffer = (unsigned char*)malloc(yuv_size);
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end, t_total_start;
    long cvt_time_us, letterbox_time_us, inference_time_us, total_time_us;
    int inference_counter = 0;
#endif
    
    printf("[推理线程] 启动\n");
    
    while(running_) {
        if (!ai_enable_) {
            usleep(100000);  // AI关闭时休眠100ms
            continue;
        }
        
        // 获取待推理帧
        bool has_frame = false;
        pthread_mutex_lock(&inference_buffer_.mutex);
        if (inference_buffer_.ready) {
            memcpy(yuv_buffer, inference_buffer_.yuv_data, yuv_size);
            inference_buffer_.ready = false;
            has_frame = true;
        }
        pthread_mutex_unlock(&inference_buffer_.mutex);
        
        if (!has_frame) {
            usleep(10000); // 增加到10ms，减少CPU空转
            continue;
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_total_start, NULL);
        gettimeofday(&t_start, NULL);
#endif
        
        // YUV转BGR（仅用于推理）
        yuv_mat = cv::Mat(height_ + height_ / 2, width_, CV_8UC1, yuv_buffer);
        bgr_mat = cv::Mat(height_, width_, CV_8UC3);
        cv::cvtColor(yuv_mat, bgr_mat, cv::COLOR_YUV420sp2BGR);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        cvt_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        // letterbox + 推理
        cv::Mat letterboxImage = letterbox(bgr_mat);
        memcpy(rknn_app_ctx_.input_mems[0]->virt_addr, letterboxImage.data, 
               model_width_ * model_height_ * 3);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        letterbox_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        inference_yolov5_model(&rknn_app_ctx_, &od_results_);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        inference_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_total_start, NULL);
        total_time_us = (t_total_start.tv_sec - t_total_start.tv_sec) * 1000000 + (t_total_start.tv_usec - t_total_start.tv_usec);
        total_time_us = cvt_time_us + letterbox_time_us + inference_time_us;
        
        inference_counter++;
        if (inference_counter % 10 == 0) {
            printf("[推理] YUV转BGR: %ld us, letterbox: %ld us, RKNN推理: %ld us, 总计: %ld us\n", 
                   cvt_time_us, letterbox_time_us, inference_time_us, total_time_us);
        }
#endif
        
        // 处理检测结果
        std::vector<DetectionInfo> temp_detections;
        
        for(int i = 0; i < od_results_.count; i++) {
            object_detect_result *det_result = &(od_results_.results[i]);
            sX = (int)(det_result->box.left);
            sY = (int)(det_result->box.top);
            eX = (int)(det_result->box.right);
            eY = (int)(det_result->box.bottom);
            mapCoordinates(&sX, &sY);
            mapCoordinates(&eX, &eY);
            
            bool drawBox = true;
            
            // 对象过滤
            if (obj_enable_ && !video_objList.empty()) {
                bool found = false;
                for (size_t j = 0; j < video_objList.size(); ++j) {
                    if (det_result->cls_id == video_objList[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) drawBox = false;
            }
            
            // 区域过滤
            if (area_enable_ && drawBox) {
                float rx = video_rectInfo.x;
                float ry = video_rectInfo.y;
                float rw = video_rectInfo.w;
                float rh = video_rectInfo.h;
                
                float left_norm = (float)sX / (float)width_;
                float top_norm = (float)sY / (float)height_;
                float right_norm = (float)eX / (float)width_;
                float bottom_norm = (float)eY / (float)height_;
                
                if (!(left_norm >= rx && right_norm <= rx+rw && 
                      top_norm >= ry && bottom_norm <= ry+rh)) {
                    drawBox = false;
                }
            }
            
            if (drawBox) {
                printf("%s @ (%d %d %d %d) %.3f\n", 
                       coco_cls_to_name(det_result->cls_id), sX, sY, eX, eY, det_result->prop);
                
                DetectionInfo detection;
                detection.cls_id = det_result->cls_id;
                detection.cls_name = coco_cls_to_name(det_result->cls_id);
                detection.x = sX;
                detection.y = sY;
                detection.w = eX - sX;
                detection.h = eY - sY;
                detection.confidence = det_result->prop;
                temp_detections.push_back(detection);
            }
        }
        
        // 更新共享检测结果
        pthread_mutex_lock(&detection_mutex_);
        shared_detections_.clear();
        shared_detections_.assign(temp_detections.begin(), temp_detections.end());
        pthread_mutex_unlock(&detection_mutex_);
        
        // 发送检测结果
        if (!temp_detections.empty() && control_) {
            static time_t last_time = 0;
            time_t now = time(NULL);
            if (last_time == 0 || (now - last_time) >= send_interval_) {
                current_detections_ = temp_detections;
                std::string summary = buildDetectionSummary();
                control_->onDetectionSummary(summary);
                printf("发送检测结果汇总 (包含%zu个物体)\n", temp_detections.size());
                last_time = now;
            }
        }
    }
    
    free(yuv_buffer);
    printf("[推理线程] 退出\n");
}

// ============ 线程3: 编码推流循环 ============
void Video::encodeLoop() {
    RK_S32 s32Ret;
    cv::Mat yuv_mat, bgr_mat;
    int yuv_size = width_ * height_ * 3 / 2;
    unsigned char* yuv_buffer = (unsigned char*)malloc(yuv_size);
    std::vector<DetectionInfo> local_detections;
    
    // FPS统计
    frame_count_ = 0;
    fps_start_time_ = time(NULL);
    current_fps_ = 0.0f;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end, t_total_start;
    long cvt_time_us, draw_time_us, encode_time_us, rtsp_time_us, total_time_us;
    int encode_counter = 0;
#endif
    
    printf("[编码线程] 启动\n");
    
    while(running_) {
        // 获取采集帧
        bool has_frame = false;
        uint64_t timestamp;
        
        pthread_mutex_lock(&capture_buffer_.mutex);
        if (capture_buffer_.ready) {
            memcpy(yuv_buffer, capture_buffer_.yuv_data, yuv_size);
            timestamp = capture_buffer_.timestamp;
            capture_buffer_.ready = false;
            has_frame = true;
        }
        pthread_mutex_unlock(&capture_buffer_.mutex);
        
        if (!has_frame) {
            usleep(1000); // 减少到1ms，更快响应
            continue;
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_total_start, NULL);
        gettimeofday(&t_start, NULL);
#endif
        
        // YUV转BGR（用于绘制）
        yuv_mat = cv::Mat(height_ + height_ / 2, width_, CV_8UC1, yuv_buffer);
        bgr_mat = cv::Mat(height_, width_, CV_8UC3, data_);
        cv::cvtColor(yuv_mat, bgr_mat, cv::COLOR_YUV420sp2BGR);
        cv::resize(bgr_mat, frame_, cv::Size(width_, height_), 0, 0, cv::INTER_LINEAR);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        cvt_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        // 获取最新检测结果
        pthread_mutex_lock(&detection_mutex_);
        local_detections.clear();
        local_detections.assign(shared_detections_.begin(), shared_detections_.end());
        pthread_mutex_unlock(&detection_mutex_);
        
        // 绘制检测框
        if (ai_enable_ && !local_detections.empty()) {
            for (const auto& det : local_detections) {
                cv::rectangle(frame_, 
                            cv::Point(det.x, det.y), 
                            cv::Point(det.x + det.w, det.y + det.h), 
                            cv::Scalar(0,255,0), 2); // 线宽从3改为2
                
                char text[64];
                sprintf(text, "%s %.0f%%", det.cls_name.c_str(), det.confidence * 100); // 去掉小数
                cv::putText(frame_, text, cv::Point(det.x, det.y - 8), 
                           cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0,255,0), 1); // 字体和线宽减小
            }
        }
        
        // 绘制区域框
        if (area_enable_) {
            int area_x = (int)(video_rectInfo.x * width_);
            int area_y = (int)(video_rectInfo.y * height_);
            int area_w = (int)(video_rectInfo.w * width_);
            int area_h = (int)(video_rectInfo.h * height_);
            
            cv::rectangle(frame_, cv::Point(area_x, area_y), 
                        cv::Point(area_x + area_w, area_y + area_h), 
                        cv::Scalar(255, 0, 0), 1); // 线宽从2改为1
            
            cv::putText(frame_, "Area", cv::Point(area_x, area_y - 8), // 简化文字
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 0), 1);
        }
        
        // 绘制FPS
        frame_count_++;
        time_t current_time = time(NULL);
        if (current_time - fps_start_time_ >= 1) {
            current_fps_ = (float)frame_count_;
            frame_count_ = 0;
            fps_start_time_ = current_time;
        }
        
        if (current_fps_ > 0.0f) {
            char fps_text[16];
            sprintf(fps_text, "%.0f", current_fps_);
            cv::putText(frame_, fps_text, 
                       cv::Point(width_ - 50, height_ / 4), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1); // 字体和线宽减小
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        draw_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        // 编码
        h264_frame_.stVFrame.u32TimeRef = H264_TimeRef_++;
        h264_frame_.stVFrame.u64PTS = timestamp;
        memcpy(data_, frame_.data, width_ * height_ * 3);
        
        RK_MPI_VENC_SendFrame(0, &h264_frame_, -1);
        s32Ret = RK_MPI_VENC_GetStream(0, &stFrame_, -1);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        encode_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        // 推流
        if(s32Ret == RK_SUCCESS && g_rtsplive_ && g_rtsp_session_) {
            void *pData = RK_MPI_MB_Handle2VirAddr(stFrame_.pstPack->pMbBlk);
            rtsp_tx_video(g_rtsp_session_, (uint8_t *)pData, 
                         stFrame_.pstPack->u32Len, stFrame_.pstPack->u64PTS);
            rtsp_do_event(g_rtsplive_);
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        rtsp_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        total_time_us = cvt_time_us + draw_time_us + encode_time_us + rtsp_time_us;
        
        encode_counter++;
        if (encode_counter % 30 == 0) {
            printf("[编码] YUV转BGR: %ld us, 绘制: %ld us, H.264编码: %ld us, RTSP推流: %ld us, 总计: %ld us\n", 
                   cvt_time_us, draw_time_us, encode_time_us, rtsp_time_us, total_time_us);
        }
#endif
        
        RK_MPI_VENC_ReleaseStream(0, &stFrame_);
    }
    
    free(yuv_buffer);
    printf("[编码线程] 退出\n");
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


void Video::getRectInfo(const RectInfo& info) {
    //拷贝一份数据到 video_rectInfo
    video_rectInfo.x = info.x;
    video_rectInfo.y = info.y;
    video_rectInfo.w = info.w;
    video_rectInfo.h = info.h;
}

void Video::getObjectList(const std::vector<int>& objList) {
    // 处理对象列表逻辑，拷贝数据到 video_objList
    video_objList.clear();
    for (const auto& obj : objList) {
        video_objList.push_back(obj);
    }
}

void Video::startAI() {
    ai_enable_ = true;
}
void Video::stopAI() {
    ai_enable_ = false;
}
void Video::startAreaDetect() {
    area_enable_ = true;
}
void Video::stopAreaDetect() {
    area_enable_ = false;
}
void Video::startObjectDetect() {
    obj_enable_ = true;
}
void Video::stopObjectDetect() {
    obj_enable_ = false;
}
void Video::startRTSP() {
    rtsp_enable_ = true;   
}
void Video::stopRTSP() {
    rtsp_enable_ = false;
}

void Video::setControl(Control* control) {
    control_ = control;
}

// 设置检测结果发送间隔（秒）
void Video::setSendInterval(int interval) {
    if (interval > 0) {
        send_interval_ = interval;
        printf("检测结果发送间隔已设置为: %d秒\n", send_interval_);
    } else {
        printf("无效的发送间隔，必须大于0\n");
    }
}

// 构建检测结果汇总字符串
std::string Video::buildDetectionSummary() {
    if (current_detections_.empty()) {
        return "DETECTIONS:NONE";
    }
    
    std::ostringstream oss;
    oss << "DETECTIONS:" << current_detections_.size();
    
    for (size_t i = 0; i < current_detections_.size(); ++i) {
        const DetectionInfo& detection = current_detections_[i];
        oss << "|" << detection.cls_id << ":" << detection.cls_name 
            << ":" << detection.x << ":" << detection.y 
            << ":" << detection.w << ":" << detection.h 
            << ":" << std::fixed << std::setprecision(3) << detection.confidence;
    }
    
    return oss.str();
}
