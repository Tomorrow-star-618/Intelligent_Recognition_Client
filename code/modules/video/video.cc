// video.cc - 视频处理模块实现
// 四线程架构：采集(30fps) → BGR转换(27fps瓶颈) → 编码推流(27fps) + 独立推理(每3帧)
// 性能：RV1106硬件限制，BGR转换37ms为瓶颈，系统稳定27fps
#include "video.h"
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <iomanip>


// 构造函数，初始化参数和三级缓冲区
Video::Video(int width, int height, int model_width, int model_height)
    : width_(width), height_(height), model_width_(model_width), model_height_(model_height),
      H264_TimeRef_(0), g_rtsplive_(NULL), g_rtsp_session_(NULL), running_(false), 
      ai_enable_(false), area_enable_(false), obj_enable_(false), 
      control_(nullptr), last_send_time_(0), send_interval_(DEFAULT_SEND_INTERVAL),
      frame_count_(0), current_fps_(0.0f),
      thread_capture_(0), thread_bgr_convert_(0), thread_inference_(0), thread_encode_(0),
      inference_frame_skip_(INFERENCE_FRAME_SKIP),  // 使用宏定义的跳帧参数
      has_detection_result_(false) {  // 初始化检测结果标志
    
    memset(&rknn_app_ctx_, 0, sizeof(rknn_app_context_t));
    memset(&fps_start_time_, 0, sizeof(struct timeval));
    memset(&program_start_time_, 0, sizeof(struct timeval));
    
    // 初始化YUV缓冲区（采集 → BGR转换）
    int yuv_size = width_ * height_ * 3 / 2;
    capture_buffer_.yuv_data = (unsigned char*)malloc(yuv_size);
    capture_buffer_.ready = false;
    pthread_mutex_init(&capture_buffer_.mutex, NULL);
    
    // 初始化BGR三缓冲区（write/ready/read指针轮换，避免拷贝阻塞）
    // BGR转换写write → 轮换到ready → 编码读read，完全无锁操作
    bgr_buffer_.bgr_write = new cv::Mat(height_, width_, CV_8UC3);
    bgr_buffer_.bgr_ready = new cv::Mat(height_, width_, CV_8UC3);
    bgr_buffer_.bgr_read = new cv::Mat(height_, width_, CV_8UC3);
    bgr_buffer_.ready = false;
    bgr_buffer_.frame_index = 0;
    bgr_buffer_.timestamp = 0;
    pthread_mutex_init(&bgr_buffer_.mutex, NULL);
    
    // 初始化推理专用BGR三缓冲区（write/ready/read指针轮换，零拷贝优化）
    // BGR转换浅拷贝到write → 轮换到ready → 推理读read，避免18ms深拷贝
    inference_bgr_buffer_.bgr_write = new cv::Mat(height_, width_, CV_8UC3);
    inference_bgr_buffer_.bgr_ready = new cv::Mat(height_, width_, CV_8UC3);
    inference_bgr_buffer_.bgr_read = new cv::Mat(height_, width_, CV_8UC3);
    inference_bgr_buffer_.ready = false; 
    inference_bgr_buffer_.frame_index = 0;
    pthread_mutex_init(&inference_bgr_buffer_.mutex, NULL);
    
    pthread_mutex_init(&detection_mutex_, NULL);
}

// 析构函数，自动释放资源
Video::~Video() {
    stop();
    
    pthread_mutex_destroy(&capture_buffer_.mutex);
    pthread_mutex_destroy(&inference_bgr_buffer_.mutex);
    pthread_mutex_destroy(&detection_mutex_);
    pthread_mutex_destroy(&bgr_buffer_.mutex);
    
    if (capture_buffer_.yuv_data) {
        free(capture_buffer_.yuv_data);
        capture_buffer_.yuv_data = NULL;
    }
    if (bgr_buffer_.bgr_write) {
        delete bgr_buffer_.bgr_write;
        bgr_buffer_.bgr_write = NULL;
    }
    if (bgr_buffer_.bgr_ready) {
        delete bgr_buffer_.bgr_ready;
        bgr_buffer_.bgr_ready = NULL;
    }
    if (bgr_buffer_.bgr_read) {
        delete bgr_buffer_.bgr_read;
        bgr_buffer_.bgr_read = NULL;
    }
    if (inference_bgr_buffer_.bgr_write) {
        delete inference_bgr_buffer_.bgr_write;
        inference_bgr_buffer_.bgr_write = NULL;
    }
    if (inference_bgr_buffer_.bgr_ready) {
        delete inference_bgr_buffer_.bgr_ready;
        inference_bgr_buffer_.bgr_ready = NULL;
    }
    if (inference_bgr_buffer_.bgr_read) {
        delete inference_bgr_buffer_.bgr_read;
        inference_bgr_buffer_.bgr_read = NULL;
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
    
    // 创建四个线程
    if (pthread_create(&thread_capture_, NULL, captureThreadFunc, this) != 0) {
        printf("[错误] 创建采集线程失败\n");
        return false;
    }
    
    if (pthread_create(&thread_bgr_convert_, NULL, bgrConvertThreadFunc, this) != 0) {
        printf("[错误] 创建BGR转换线程失败\n");
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
    
    printf("[成功] 四线程启动: 采集、BGR转换、推理、编码推流\n");
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
    if (thread_bgr_convert_) {
        pthread_join(thread_bgr_convert_, NULL);
        thread_bgr_convert_ = 0;
        printf("[线程] BGR转换线程已停止\n");
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

void* Video::bgrConvertThreadFunc(void* arg) {
    Video* self = static_cast<Video*>(arg);
    self->bgrConvertLoop();
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
// 功能: 从VI硬件获取YUV帧，快速拷贝到capture_buffer
// 性能: VI获取30ms + YUV拷贝2ms = 32ms → 稳定30fps
// 特点: 不等待消费，直接覆盖旧数据，保证实时性（允许下游丢帧）
void Video::captureLoop() {
    RK_S32 s32Ret;
    int frame_counter = 0;
    int yuv_size = width_ * height_ * 3 / 2;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end;
    long vi_time_us, copy_time_us, total_time_us;
#endif
    
    printf("[采集线程] 启动 - YUV快速拷贝模式（30fps）\n");
    
    while(running_) {
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame_, -1);
        if(s32Ret != RK_SUCCESS) {
            continue; // 立即重试
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        vi_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame_.stVFrame.pMbBlk);
        uint64_t timestamp = TEST_COMM_GetNowUs();
        
        // 🔒 快速拷贝YUV到BGR转换缓冲区（每帧都要，~2ms，加锁保护）
        // 注意：不检查ready标志，直接覆盖，BGR转换线程会丢帧（保证采集30fps实时性）
        pthread_mutex_lock(&capture_buffer_.mutex);
        memcpy(capture_buffer_.yuv_data, vi_data, yuv_size);
        capture_buffer_.timestamp = timestamp;
        capture_buffer_.ready = true;
        pthread_mutex_unlock(&capture_buffer_.mutex);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        copy_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        total_time_us = vi_time_us + copy_time_us;
        
        // 每30帧打印一次
        if (frame_counter % 30 == 0) {
            printf("[采集#%d] VI获取: %ld us, YUV拷贝: %ld us, 总计: %ld us\n", 
                   frame_counter, vi_time_us, copy_time_us, total_time_us);
        }
#endif
        
        RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame_);
        frame_counter++;
    }
    
    printf("[采集线程] 退出\n");
}

// ============ 线程2: BGR转换循环 ============
// 功能: 从capture_buffer获取YUV，转换为BGR，同时输出给编码和推理线程
// 性能: YUV→BGR 32ms（RV1106无GPU硬件限制，瓶颈） + 指针轮换<1us + 推理拷贝10ms
// 特点: 转换在write缓冲区完成（无锁），轮换时仅交换指针（极快），每N帧拷贝给推理
void Video::bgrConvertLoop() {
    int yuv_size = width_ * height_ * 3 / 2;
    unsigned char* yuv_buffer = (unsigned char*)malloc(yuv_size);
    cv::Mat yuv_mat;
    int frame_counter = 0;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end;
    long cvt_time_us, swap_time_us, inference_copy_time_us = 0;
#endif
    
    printf("[BGR转换线程] 启动 - 三缓冲指针轮换模式 + 推理BGR共享\n");
    
    while(running_) {
        bool has_frame = false;
        
        // 🔒 从capture_buffer获取YUV数据（加锁保护）
        pthread_mutex_lock(&capture_buffer_.mutex);
        if (capture_buffer_.ready) {
            memcpy(yuv_buffer, capture_buffer_.yuv_data, yuv_size);
            has_frame = true;
            capture_buffer_.ready = false;  // 标记已消费
        }
        pthread_mutex_unlock(&capture_buffer_.mutex);
        
        if (!has_frame) {
            usleep(1000); // 1ms
            continue;
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        // YUV→BGR转换（无锁，直接写入write缓冲区，32ms为系统瓶颈）
        yuv_mat = cv::Mat(height_ + height_ / 2, width_, CV_8UC1, yuv_buffer);
        cv::cvtColor(yuv_mat, *bgr_buffer_.bgr_write, cv::COLOR_YUV420sp2BGR);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        cvt_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        // 🔒 三缓冲轮换：write→ready→read→write（加锁，仅轮换指针 <1us）
        // 零拷贝技术：仅交换8字节指针，比拷贝2.7MB数据快数万倍
        pthread_mutex_lock(&bgr_buffer_.mutex);
        cv::Mat* temp = bgr_buffer_.bgr_ready;  // 保存旧的ready
        bgr_buffer_.bgr_ready = bgr_buffer_.bgr_write;  // write提升为ready
        bgr_buffer_.bgr_write = temp;  // 旧ready降为write
        bgr_buffer_.timestamp = TEST_COMM_GetNowUs();
        bgr_buffer_.frame_index = frame_counter;
        bgr_buffer_.ready = true;
        pthread_mutex_unlock(&bgr_buffer_.mutex);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        swap_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
#endif
        
        // 🔒 每N帧传递BGR数据给推理线程（三缓冲指针轮换，零拷贝优化）
        // 优化：浅拷贝Mat头到write缓冲，然后指针轮换，避免18ms深拷贝
        if (ai_enable_ && (frame_counter % (inference_frame_skip_ + 1)) == 0) {
#ifdef ENABLE_PERFORMANCE_TIMING
            gettimeofday(&t_start, NULL);
#endif
            // 浅拷贝：仅复制Mat头（48字节），共享数据指针（无锁，安全）
            *inference_bgr_buffer_.bgr_write = *bgr_buffer_.bgr_ready;
            
            // 三缓冲指针轮换（锁内<1us）
            pthread_mutex_lock(&inference_bgr_buffer_.mutex);
            if (!inference_bgr_buffer_.ready) {  // 避免覆盖未处理的帧
                cv::Mat* temp = inference_bgr_buffer_.bgr_ready;
                inference_bgr_buffer_.bgr_ready = inference_bgr_buffer_.bgr_write;
                inference_bgr_buffer_.bgr_write = temp;
                inference_bgr_buffer_.frame_index = frame_counter;
                inference_bgr_buffer_.ready = true;
            }
            pthread_mutex_unlock(&inference_bgr_buffer_.mutex);
            
#ifdef ENABLE_PERFORMANCE_TIMING
            gettimeofday(&t_end, NULL);
            inference_copy_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
#endif
        } else {
            inference_copy_time_us = 0;
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        if (frame_counter % 30 == 0) {
            long total_us = cvt_time_us + swap_time_us + inference_copy_time_us;
            printf("[BGR转换#%d] YUV→BGR: %ld us, 指针轮换: %ld us, 推理轮换: %ld us, 总计: %ld us\n", 
                   frame_counter, cvt_time_us, swap_time_us, inference_copy_time_us, total_us);
        }
#endif
        
        frame_counter++;
    }
    
    free(yuv_buffer);
    printf("[BGR转换线程] 退出\n");
}

// ============ 线程3: 推理循环 ============
// 功能: 从inference_bgr_buffer获取BGR数据，letterbox缩放，RKNN推理
// 性能: letterbox 18ms + 推理 96ms = 114ms（每5帧推理1次，三缓冲零拷贝优化）
// 特点: 独立运行，不影响编码线程，结果通过detection_mutex_共享
void Video::inferenceLoop() {
    cv::Mat bgr_mat;
    int sX, sY, eX, eY;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end;
    long swap_time_us, letterbox_time_us, inference_time_us, total_time_us;
    int inference_counter = 0;
#endif
    
    printf("[推理线程] 启动 - 三缓冲指针交换模式（零拷贝优化）\n");
    
    while(running_) {
        if (!ai_enable_) {
            usleep(100000);  // AI关闭时休眠100ms
            continue;
        }
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        // 🔒 三缓冲指针交换获取待推理BGR帧（锁内<1us，零拷贝）
        bool has_frame = false;
        pthread_mutex_lock(&inference_bgr_buffer_.mutex);
        if (inference_bgr_buffer_.ready) {
            cv::Mat* temp = inference_bgr_buffer_.bgr_read;
            inference_bgr_buffer_.bgr_read = inference_bgr_buffer_.bgr_ready;
            inference_bgr_buffer_.bgr_ready = temp;
            inference_bgr_buffer_.ready = false;
            has_frame = true;
        }
        pthread_mutex_unlock(&inference_bgr_buffer_.mutex);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        swap_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
#endif
        
        if (!has_frame) {
            usleep(10000); // 10ms
            continue;
        }
        
        // 使用read缓冲（无锁，推理线程独占）
        bgr_mat = *inference_bgr_buffer_.bgr_read;
        
#ifdef ENABLE_PERFORMANCE_TIMING
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
        total_time_us = swap_time_us + letterbox_time_us + inference_time_us;
        
        inference_counter++;
        if (inference_counter % 10 == 0) {
            printf("[推理#%d] 指针交换: %ld us, letterbox: %ld us, RKNN推理: %ld us, 总计: %ld us\n", 
                   inference_counter, swap_time_us, letterbox_time_us, inference_time_us, total_time_us);
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
                // printf("%s @ (%d %d %d %d) %.3f\n", 
                //        coco_cls_to_name(det_result->cls_id), sX, sY, eX, eY, det_result->prop);
                
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
        
        // 🔒 更新共享检测结果（加锁保护）
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
                // printf("发送检测结果汇总 (包含%zu个物体)\n", temp_detections.size());
                last_time = now;
            }
        }
    }
    
    printf("[推理线程] 退出\n");
}

// ============ 线程4: 编码推流循环 ============
// 功能: 从BGR三缓冲区获取ready帧，绘制检测框+FPS，H.264编码，RTSP推流
// 性能: 指针交换<1us + 绘制2ms + H.264编码12ms + RTSP 1ms = 15ms
// 特点: 受限于BGR转换速度(37ms)，实际输出27fps，FPS在此线程统计
void Video::encodeLoop() {
    RK_S32 s32Ret;
    int last_processed_index = -1;  // 上次处理的帧序号（避免重复）
    
    // FPS统计
    frame_count_ = 0;
    gettimeofday(&fps_start_time_, NULL);
    gettimeofday(&program_start_time_, NULL);  // 记录程序启动时间
    current_fps_ = 0.0f;
    
#ifdef ENABLE_PERFORMANCE_TIMING
    struct timeval t_start, t_end;
    long swap_time_us, draw_time_us, encode_time_us, rtsp_time_us, total_time_us;
    int encode_counter = 0;
#endif
    
    printf("[编码线程] 启动 - 三缓冲模式，从BGR转换线程获取\n");
    
    while(running_) {
        // 🔒 从BGR缓冲区交换ready→read指针（加锁保护，允许跳帧）
        // 三缓冲优势：编码线程使用read缓冲期间，BGR转换可同时写write缓冲
        bool has_frame = false;
        uint64_t timestamp;
        int current_frame_index;
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_start, NULL);
#endif
        
        pthread_mutex_lock(&bgr_buffer_.mutex);
        if (bgr_buffer_.ready) {
            // 交换ready和read指针（无论是否处理过，都交换以获取最新帧）
            cv::Mat* temp = bgr_buffer_.bgr_read;
            bgr_buffer_.bgr_read = bgr_buffer_.bgr_ready;
            bgr_buffer_.bgr_ready = temp;
            
            timestamp = bgr_buffer_.timestamp;
            current_frame_index = bgr_buffer_.frame_index;
            bgr_buffer_.ready = false;  // 标记已消费
            has_frame = true;
            last_processed_index = current_frame_index;  // 更新已处理帧序号
        }
        pthread_mutex_unlock(&bgr_buffer_.mutex);
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        swap_time_us = (t_end.tv_sec - t_start.tv_sec) * 1000000 + (t_end.tv_usec - t_start.tv_usec);
        gettimeofday(&t_start, NULL);
#endif
        
        if (!has_frame) {
            usleep(1000); // 1ms
            continue;
        }
        
        // 使用read缓冲区的BGR数据（浅拷贝，共享数据指针，无性能损失）
        frame_ = *bgr_buffer_.bgr_read;
        
#ifdef ENABLE_PERFORMANCE_TIMING
        gettimeofday(&t_end, NULL);
        // swap_time已包含在上面，这里不额外计时
        gettimeofday(&t_start, NULL);
#endif
        
        // 🔒 获取最新检测结果（加锁保护，仅读取）
        pthread_mutex_lock(&detection_mutex_);
        if (!shared_detections_.empty()) {
            // 有新推理结果，更新缓存
            cached_detections_.clear();
            cached_detections_.assign(shared_detections_.begin(), shared_detections_.end());
            has_detection_result_ = true;
        }
        pthread_mutex_unlock(&detection_mutex_);
        
        // 绘制检测框（使用缓存结果，每帧都绘制）
        if (ai_enable_ && has_detection_result_ && !cached_detections_.empty()) {
            for (const auto& det : cached_detections_) {
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
        
        // ✅ 精确FPS计算（使用微秒级时间）
        frame_count_++;
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        
        // 计算距离上次FPS更新的时间差（秒）
        double time_diff = (current_time.tv_sec - fps_start_time_.tv_sec) + 
                          (current_time.tv_usec - fps_start_time_.tv_usec) / 1000000.0;
        
        if (time_diff >= 1.0) {
            current_fps_ = (float)frame_count_ / time_diff;  // 精确FPS
            printf("[FPS统计] 实际处理帧数: %d, 时间差: %.3f秒, FPS: %.1f\n", 
                   frame_count_, time_diff, current_fps_);
            frame_count_ = 0;
            fps_start_time_ = current_time;
        }
        
        // 计算运行时间（分:秒.毫秒）
        double runtime = (current_time.tv_sec - program_start_time_.tv_sec) + 
                        (current_time.tv_usec - program_start_time_.tv_usec) / 1000000.0;
        int minutes = (int)(runtime / 60);
        int seconds = (int)runtime % 60;
        int milliseconds = (int)((runtime - (int)runtime) * 1000);
        
        // 绘制FPS和运行时间
        if (current_fps_ > 0.0f) {
            char fps_text[32];
            sprintf(fps_text, "%.1f", current_fps_);  // 显示1位小数
            cv::putText(frame_, fps_text, 
                       cv::Point(width_ - 60, height_ / 4), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
            
            // 在FPS下方显示运行时间
            char time_text[32];
            sprintf(time_text, "%02d:%02d.%03d", minutes, seconds, milliseconds);
            cv::putText(frame_, time_text, 
                       cv::Point(width_ - 90, height_ / 4 + 25), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 255, 255), 1);
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
        total_time_us = swap_time_us + draw_time_us + encode_time_us + rtsp_time_us;
        
        encode_counter++;
        if (encode_counter % 30 == 0) {
            printf("[编码#%d] 指针交换: %ld us, 绘制: %ld us, H.264: %ld us, RTSP: %ld us, 总计: %ld us\n", 
                   encode_counter, swap_time_us, draw_time_us, encode_time_us, rtsp_time_us, total_time_us);
        }
#endif
        
        RK_MPI_VENC_ReleaseStream(0, &stFrame_);
    }
    
    printf("[编码线程] 退出\n");
}

// letterbox处理：缩放+填充，适配模型输入
// 优化：使用INTER_NEAREST插值（比INTER_LINEAR快2-5倍）+ memset填充黑边
cv::Mat Video::letterbox(cv::Mat input) {
    float scaleX = (float)model_width_ / (float)width_;
    float scaleY = (float)model_height_ / (float)height_;
    scale_ = scaleX < scaleY ? scaleX : scaleY;
    int inputWidth = (int)((float)width_ * scale_);
    int inputHeight = (int)((float)height_ * scale_);
    leftPadding_ = (model_width_ - inputWidth) / 2;
    topPadding_ = (model_height_ - inputHeight) / 2;
    cv::Mat inputScale;
    cv::resize(input, inputScale, cv::Size(inputWidth,inputHeight), 0, 0, cv::INTER_NEAREST);
    cv::Mat letterboxImage(model_height_, model_width_, CV_8UC3);
    memset(letterboxImage.data, 0, model_width_ * model_height_ * 3);
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
