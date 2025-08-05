// control.cc - 控制命令解析与分发模块实现
#include "control.h"
#include <stdio.h>
#include <sstream>

// 构造函数，保存Servo和Video指针副本
Control::Control(Servo* servoPtr, Video* videoPtr)
    : m_servo(servoPtr ? new Servo(*servoPtr) : nullptr), m_video(videoPtr) {}

// 解析命令字符串并分发
void Control::parseAndDispatch(const std::string& cmd) {
    int deviceId = 0, operationId = 0, operationValue = 0;
    // 支持格式: DEVICE_1:OP_1:VALUE_5
    if (sscanf(cmd.c_str(), "DEVICE_%d:OP_%d:VALUE_%d", &deviceId, &operationId, &operationValue) == 3) {
        if (deviceId == 1 && m_servo) {
            if (operationId >= 1 && operationId <= 4) {
                // 1=UP, 2=DOWN, 3=LEFT, 4=RIGHT
                int direction = -1;
                switch (operationId) {
                    case 1: direction = SERVO_UP; break;
                    case 2: direction = SERVO_DOWN; break;
                    case 3: direction = SERVO_LEFT; break;
                    case 4: direction = SERVO_RIGHT; break;
                }
                // 默认垂直舵机UP/DOWN，水平舵机LEFT/RIGHT
                int servo_type = (direction == SERVO_LEFT || direction == SERVO_RIGHT) ? SERVO_HORIZONTAL : SERVO_VERTICAL;
                m_servo->control(servo_type, direction, operationValue);
            } else if (operationId == 5) {
                m_servo->reset();
            } else {
                printf("未支持的operationId: %d\n", operationId);
            }
        } else if (deviceId == 2 && m_video) {
            if (operationId == 6) {
                if (operationValue == 1) {
                    m_video->startAI();
                    printf("AI识别已开启\n");
                } else if (operationValue == 0) {
                    m_video->stopAI();
                    printf("AI识别已关闭\n");
                } else {
                    printf("未支持的operationValue: %d\n", operationValue);
                }
            } else {
                printf("未支持的operationId: %d\n", operationId);
            }
        } else {
            printf("未支持的deviceId: %d\n", deviceId);
        }
    } else {
        printf("命令格式错误: %s\n", cmd.c_str());
    }
}
