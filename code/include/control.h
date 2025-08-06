// control.h - 控制命令解析与分发模块头文件
#ifndef CONTROL_H
#define CONTROL_H

#include <string>
#include "common.h"
#include "servo.h"
#include "video.h"

// Control类：负责解析命令字符串并分发
class Control {
public:
    // 构造时传入Servo和Video指针
    Control(Servo* servoPtr, Video* videoPtr);

    // 解析命令字符串并分发
    void parseAndDispatch(const std::string& cmd);
    // 解析区域识别命令
    void parseRectInfo(const std::string& rectCmd);


private:
    Servo* m_servo;
    Video* m_video;
    RectInfo m_rectInfo; // 保存最新的矩形框数据
};

#endif // CONTROL_H
