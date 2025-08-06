#ifndef _RKNN_YOLOV5_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV5_DEMO_POSTPROCESS_H_

#include <stdint.h>
#include <vector>
#include "rknn_api.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 80
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)

// class rknn_app_context_t;
// 目标检测框结构，表示一个矩形区域
typedef struct {
    int left;   // 检测框左边界x坐标
    int top;    // 检测框上边界y坐标
    int right;  // 检测框右边界x坐标
    int bottom; // 检测框下边界y坐标
} image_rect_t;

// 单个目标的检测结果
typedef struct {
    image_rect_t box; // 目标框位置和大小
    float prop;       // 置信度（概率，0~1）
    int cls_id;       // 类别ID（如0=person，1=bicycle等）
} object_detect_result;

// 一帧图像的所有检测结果列表
typedef struct {
    int id;    // 结果ID或帧序号（可选）
    int count; // 检测到的目标数量
    object_detect_result results[OBJ_NUMB_MAX_SIZE]; // 所有目标的检测结果数组
} object_detect_result_list;

int init_post_process();
void deinit_post_process();
char *coco_cls_to_name(int cls_id);
int post_process(rknn_app_context_t *app_ctx, void *outputs,  float conf_threshold, float nms_threshold, object_detect_result_list *od_results);

void deinitPostProcess();
#endif //_RKNN_YOLOV5_DEMO_POSTPROCESS_H_
