// main.cc 详细中文注释版
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "yolov5.h"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#define DISP_WIDTH  720   // 显示宽度
#define DISP_HEIGHT 480   // 显示高度

// 显示尺寸
int width    = DISP_WIDTH;
int height   = DISP_HEIGHT;

// 模型输入尺寸
int model_width = 640;
int model_height = 640;	
float scale ;           // 缩放比例
int leftPadding ;       // 左侧填充
int topPadding  ;       // 顶部填充

// 图像letterbox处理，将原始图像缩放并填充到模型输入尺寸
cv::Mat letterbox(cv::Mat input)
{
	float scaleX = (float)model_width  / (float)width; 
	float scaleY = (float)model_height / (float)height; 
	scale = scaleX < scaleY ? scaleX : scaleY;
	
	int inputWidth   = (int)((float)width * scale);
	int inputHeight  = (int)((float)height * scale);

	leftPadding = (model_width  - inputWidth) / 2;
	topPadding  = (model_height - inputHeight) / 2;	

	cv::Mat inputScale;
    cv::resize(input, inputScale, cv::Size(inputWidth,inputHeight), 0, 0, cv::INTER_LINEAR);	
    // 创建黑色背景
	cv::Mat letterboxImage(640, 640, CV_8UC3,cv::Scalar(0, 0, 0));
    cv::Rect roi(leftPadding, topPadding, inputWidth, inputHeight);
    inputScale.copyTo(letterboxImage(roi));

	return letterboxImage; 	
}

// 将模型输出的坐标映射回原图坐标
void mapCoordinates(int *x, int *y) {	
	int mx = *x - leftPadding;
	int my = *y - topPadding;
    *x = (int)((float)mx / scale);
    *y = (int)((float)my / scale);
}

// 主程序入口
int main(int argc, char *argv[]) {
    system("RkLunch-stop.sh"); // 停止可能占用资源的进程
	RK_S32 s32Ret = 0; 
	int sX,sY,eX,eY; 
		
    // rknn模型相关变量
	char text[16];
	rknn_app_context_t rknn_app_ctx;	
	object_detect_result_list od_results;
    int ret;
	const char *model_path = "./model/yolov5.rknn";
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));	
    init_yolov5_model(model_path, &rknn_app_ctx); // 初始化yolov5模型
	printf("init rknn model success!\n");
    init_post_process(); // 初始化后处理

    // H264编码帧结构体
	VENC_STREAM_S stFrame;	
	stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
	RK_U64 H264_PTS = 0;
	RK_U32 H264_TimeRef = 0; 
	VIDEO_FRAME_INFO_S stViFrame;
	
    // 创建内存池
	MB_POOL_CONFIG_S PoolCfg;
	memset(&PoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    PoolCfg.u64MBSize = width * height * 3 ; // RGB888
	PoolCfg.u32MBCnt = 1;
	PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
	MB_POOL src_Pool = RK_MPI_MB_CreatePool(&PoolCfg);
	printf("Create Pool success !\n");	

    // 从内存池获取内存块
	MB_BLK src_Blk = RK_MPI_MB_GetMB(src_Pool, width * height * 3, RK_TRUE);
	
    // 构建h264编码帧
	VIDEO_FRAME_INFO_S h264_frame;
	h264_frame.stVFrame.u32Width = width;
	h264_frame.stVFrame.u32Height = height;
	h264_frame.stVFrame.u32VirWidth = width;
	h264_frame.stVFrame.u32VirHeight = height;
	h264_frame.stVFrame.enPixelFormat =  RK_FMT_RGB888; 
	h264_frame.stVFrame.u32FrameFlag = 160;
	h264_frame.stVFrame.pMbBlk = src_Blk;
	unsigned char *data = (unsigned char *)RK_MPI_MB_Handle2VirAddr(src_Blk);
    cv::Mat frame(cv::Size(width,height),CV_8UC3,data); // 用于OpenCV操作的Mat

    // rkaiq ISP初始化
	RK_BOOL multi_sensor = RK_FALSE;	
	const char *iq_dir = "/etc/iqfiles";
	rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
	//hdr_mode = RK_AIQ_WORKING_MODE_ISP_HDR2;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir); // ISP初始化
    SAMPLE_COMM_ISP_Run(0); // ISP运行

    // rkmpi系统初始化
	if (RK_MPI_SYS_Init() != RK_SUCCESS) {
		RK_LOGE("rk mpi sys init fail!");
		return -1;
	}

    // RTSP推流初始化
	rtsp_demo_handle g_rtsplive = NULL;
	rtsp_session_handle g_rtsp_session;
    g_rtsplive = create_rtsp_demo(554); // 创建RTSP服务，端口554
    g_rtsp_session = rtsp_new_session(g_rtsplive, "/live/0"); // 新建会话
    rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0); // 设置视频编码格式
    rtsp_sync_video_ts(g_rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime()); // 同步时间戳
	
    // VI（视频输入）初始化
	vi_dev_init();
	vi_chn_init(0, width, height);

    // VENC（视频编码）初始化
	RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
	venc_init(0, width, height, enCodecType);

	printf("venc init success\n");	
	
  	// 主循环：不断采集、推理、编码、推流
  	while(1)
	{	
        // 1. 获取一帧视频输入（VI）
		h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;                  // 帧参考计数递增
		h264_frame.stVFrame.u64PTS = TEST_COMM_GetNowUs();                // 获取当前时间戳
		s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, -1);             // 从VI通道获取一帧
		if(s32Ret == RK_SUCCESS)
		{
			void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);	// 获取物理内存虚拟地址

            // 2. YUV转BGR，便于OpenCV处理
			cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);       // 构造YUV420SP格式Mat
			cv::Mat bgr(height, width, CV_8UC3, data);                            // 构造BGR格式Mat，data为编码内存
			cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);                  // YUV420SP转BGR
			cv::resize(bgr, frame, cv::Size(width ,height), 0, 0, cv::INTER_LINEAR); // 调整尺寸到frame

            // 3. letterbox处理，适配模型输入尺寸
			cv::Mat letterboxImage = letterbox(frame);	                          // 缩放+填充，letterbox到模型输入
            memcpy(rknn_app_ctx.input_mems[0]->virt_addr, letterboxImage.data, model_width*model_height*3); // 拷贝到模型输入内存
            inference_yolov5_model(&rknn_app_ctx, &od_results);                   // 进行yolov5推理

            // 4. 处理推理结果，画框和标签
			for(int i = 0; i < od_results.count; i++)
			{					
				if(od_results.count >= 1)
				{
					object_detect_result *det_result = &(od_results.results[i]);
	
					sX = (int)(det_result->box.left   );	// 检测框左上角X
                    sY = (int)(det_result->box.top    );    // 检测框左上角Y
					eX = (int)(det_result->box.right  );	// 检测框右下角X
					eY = (int)(det_result->box.bottom );    // 检测框右下角Y
                    mapCoordinates(&sX,&sY);                // 映射回原图坐标
					mapCoordinates(&eX,&eY);
					
					printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id),
							 sX, sY, eX, eY, det_result->prop);

                    // 画检测框
					cv::rectangle(frame,cv::Point(sX ,sY),
								        cv::Point(eX ,eY),
										cv::Scalar(0,255,0),3);
                    // 画类别标签
					sprintf(text, "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
					cv::putText(frame,text,cv::Point(sX, sY - 8),
										   cv::FONT_HERSHEY_SIMPLEX,1,
										   cv::Scalar(0,255,0),2);
				}
			}
		}
        // 5. 将处理后图像拷贝回编码内存
        memcpy(data, frame.data, width * height * 3); 
		
        // 6. 送入H264编码器
		RK_MPI_VENC_SendFrame(0, &h264_frame,-1);

        // 7. 获取编码后码流并推送到RTSP
		s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
		if(s32Ret == RK_SUCCESS)
		{
			if(g_rtsplive && g_rtsp_session)
			{
                // 获取编码数据并推送到RTSP
				void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
				rtsp_tx_video(g_rtsp_session, (uint8_t *)pData, stFrame.pstPack->u32Len,
							  stFrame.pstPack->u64PTS);
				rtsp_do_event(g_rtsplive); // 处理RTSP事件
			}
		}

        // 8. 释放VI帧资源
		s32Ret = RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
		if (s32Ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);
		}
        // 9. 释放编码帧资源
		s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
		if (s32Ret != RK_SUCCESS) {
			RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
		}
		memset(text,0,8); // 清空标签文本缓存
	}

    // 销毁内存块
	RK_MPI_MB_ReleaseMB(src_Blk);
    // 销毁内存池
	RK_MPI_MB_DestroyPool(src_Pool);
	
    // 关闭VI
	RK_MPI_VI_DisableChn(0, 0);
	RK_MPI_VI_DisableDev(0);

    // 停止ISP
	SAMPLE_COMM_ISP_Stop(0);
	
    // 关闭编码器
	RK_MPI_VENC_StopRecvFrame(0);
	RK_MPI_VENC_DestroyChn(0);

	free(stFrame.pstPack);

    // 关闭RTSP
	if (g_rtsplive)
		rtsp_del_demo(g_rtsplive);
	
    // 关闭系统
	RK_MPI_SYS_Exit();

    // 释放rknn模型
    release_yolov5_model(&rknn_app_ctx);		
	deinit_post_process();
	
	return 0;
}
