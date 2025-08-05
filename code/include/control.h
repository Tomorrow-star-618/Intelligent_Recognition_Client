// control.h - 控制命令解析与分发模块头文件
#ifndef CONTROL_H
#define CONTROL_H

#include <string>
#include "servo.h"
#include "video.h"

// Control类：负责解析命令字符串并分发到对应的处理函数
class Control {
public:
    // 构造时传入Servo和Video指针
    Control(Servo* servoPtr, Video* videoPtr);

    // 解析命令字符串并分发
    void parseAndDispatch(const std::string& cmd);

private:
    Servo* m_servo;
    Video* m_video;
};

#endif // CONTROL_H
