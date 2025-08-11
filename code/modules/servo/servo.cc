// servo.cc - 舵机控制模块实现
#include "servo.h"
#include "pwm.h"
#include <stdio.h>
#include <unistd.h>

Servo::Servo() {}
Servo::~Servo() {}

// 控制舵机转动
int Servo::control(int servo_type, int direction, int value) {
    const char* pwm_path = nullptr;
    int delta = value * 100000;
    int duty_cycle = 0;
    int ret = 0;

    // 选择舵机类型
    if (servo_type == SERVO_VERTICAL) {
        pwm_path = PWM10_PATH;
    } else if (servo_type == SERVO_HORIZONTAL) {
        pwm_path = PWM11_PATH;
    } else {
        printf("未知舵机类型: %d\n", servo_type);
        return -1;
    }

    // 初始化PWM
    ret = pwm_init(pwm_path);
    if (ret != 0) {
        printf("PWM初始化失败\n");
        return -1;
    }

    // 读取当前占空比
    duty_cycle = pwm_read_duty_cycle(pwm_path);
    if (duty_cycle < 0) {
        printf("读取占空比失败\n");
        pwm_release(pwm_path);
        return -1;
    }

    // 根据方向调整占空比
    if (servo_type == SERVO_VERTICAL) {
        if (direction == SERVO_UP) {
            duty_cycle -= delta;
        } else if (direction == SERVO_DOWN) {
            duty_cycle += delta;
        } else {
            printf("垂直舵机方向错误\n");
            pwm_release(pwm_path);
            return -1;
        }
    } else if (servo_type == SERVO_HORIZONTAL) {
        if (direction == SERVO_LEFT) {
            duty_cycle -= delta;
        } else if (direction == SERVO_RIGHT) {
            duty_cycle += delta;
        } else {
            printf("水平舵机方向错误\n");
            pwm_release(pwm_path);
            return -1;
        }
    }

    // 设置新的占空比
    ret = pwm_set_duty_cycle(pwm_path, duty_cycle);
    if (ret != 0) {
        printf("设置占空比失败\n");
        pwm_release(pwm_path);
        return -1;
    }

    // 使能PWM
    ret = pwm_enable(pwm_path, 1);
    if (ret != 0) {
        printf("PWM使能失败\n");
        pwm_release(pwm_path);
        return -1;
    }

    // 等待一段时间让舵机转动
    usleep(500000); // 等待500ms

    // 取消使能PWM
    ret = pwm_enable(pwm_path, 0);
    if (ret != 0) {
        printf("PWM使能失败\n");
        pwm_release(pwm_path);
        return -1;
    }

    // 释放PWM
    pwm_release(pwm_path);
    printf("舵机控制完成: type=%d, dir=%d, value=%d\n", servo_type, direction, value);
    return 0;
}

// 舵机复位
int Servo::reset() {
    int ret = 0;
    // 依次复位垂直舵机
    if (pwm_init(PWM10_PATH) != 0) {
        printf("PWM10初始化失败\n");
        return -1;
    }
    if (pwm_set_duty_cycle(PWM10_PATH, 1500000) != 0) {
        printf("PWM10设置占空比失败\n");
        pwm_release(PWM10_PATH);
        return -1;
    }
    if (pwm_enable(PWM10_PATH, 1) != 0) {
        printf("PWM10使能失败\n");
        pwm_release(PWM10_PATH);
        return -1;
    }
    pwm_release(PWM10_PATH);

    // 依次复位水平舵机
    if (pwm_init(PWM11_PATH) != 0) {
        printf("PWM11初始化失败\n");
        return -1;
    }
    if (pwm_set_duty_cycle(PWM11_PATH, 1500000) != 0) {
        printf("PWM11设置占空比失败\n");
        pwm_release(PWM11_PATH);
        return -1;
    }
    if (pwm_enable(PWM11_PATH, 1) != 0) {
        printf("PWM11使能失败\n");
        pwm_release(PWM11_PATH);
        return -1;
    }
    pwm_release(PWM11_PATH);

    printf("舵机已复位到duty_cycle_ns=1500000\n");
    return 0;
}
