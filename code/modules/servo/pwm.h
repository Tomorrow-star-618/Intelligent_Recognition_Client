#ifndef _PWM_H_
#define _PWM_H_

#ifdef __cplusplus
extern "C" {
#endif

#define PWM10_PATH "/sys/class/pwm/pwmchip10" //垂直舵机，数值从小到大从上往下
#define PWM11_PATH "/sys/class/pwm/pwmchip11" //水平舵机，数值从左往右从小到大

#define PERIOD_NS          20000000
#define MIN_DUTY_CYCLE_NS    500000
#define MAX_DUTY_CYCLE_NS   2500000

// 初始化PWM，pwm_path为"/sys/class/pwm/pwmchip10"或"/sys/class/pwm/pwmchip11"
int pwm_init(const char *pwm_path);

// 设置PWM占空比
int pwm_set_duty_cycle(const char *pwm_path, int duty_cycle_ns);

// 读取PWM当前占空比，返回duty_cycle_ns，失败返回-1
int pwm_read_duty_cycle(const char *pwm_path);

// 使能PWM
int pwm_enable(const char *pwm_path, int enable);

// 释放PWM
int pwm_release(const char *pwm_path);

#ifdef __cplusplus
}
#endif

#endif // _PWM_H_