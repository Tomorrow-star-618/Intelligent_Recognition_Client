// servo.h - 舵机控制模块头文件
#ifndef SERVO_H
#define SERVO_H

// 舵机类型
#define SERVO_VERTICAL   0  // 垂直舵机
#define SERVO_HORIZONTAL 1  // 水平舵机

// 舵机方向
#define SERVO_UP    0  // 垂直向上
#define SERVO_DOWN  1  // 垂直向下
#define SERVO_LEFT  2  // 水平向左
#define SERVO_RIGHT 3  // 水平向右

class Servo {
public:
    Servo();
    ~Servo();
    // 控制舵机转动
    int control(int servo_type, int direction, int value);
    // 舵机复位
    int reset();
};

#endif // SERVO_H
