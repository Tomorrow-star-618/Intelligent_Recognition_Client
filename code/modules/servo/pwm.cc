#include "pwm.h"
#include <stdio.h>
#include <stdlib.h>

int pwm_init(const char *pwm_path) {
    char buf[128];
    FILE *pwm_export;

    // 导出PWM到用户空间
    snprintf(buf, sizeof(buf), "%s/export", pwm_path);
    pwm_export = fopen(buf, "w");
    if (!pwm_export) {
        perror("Failed to open PWM export");
        return 1;
    }
    fprintf(pwm_export, "0");
    fclose(pwm_export);

    // 设置周期为20ms，频率为50Hz
    snprintf(buf, sizeof(buf), "%s/pwm0/period", pwm_path);
    FILE *period_file = fopen(buf, "w");
    if (!period_file) {
        perror("Failed to open PWM period");
        return 1;
    }
    fprintf(period_file, "%d", PERIOD_NS);
    fclose(period_file);

    // 设置极性为 normal
    snprintf(buf, sizeof(buf), "%s/pwm0/polarity", pwm_path);
    FILE *polarity_file = fopen(buf, "w");
    if (!polarity_file) {
        perror("Failed to open PWM polarity");
        return 1;
    }
    fprintf(polarity_file, "%s", "normal");
    fclose(polarity_file);

    return 0;
}

int pwm_set_duty_cycle(const char *pwm_path, int duty_cycle_ns) {
    char buf[128];
    if (duty_cycle_ns < MIN_DUTY_CYCLE_NS) duty_cycle_ns = MIN_DUTY_CYCLE_NS;
    if (duty_cycle_ns > MAX_DUTY_CYCLE_NS) duty_cycle_ns = MAX_DUTY_CYCLE_NS;
    // 设置占空比
    snprintf(buf, sizeof(buf), "%s/pwm0/duty_cycle", pwm_path);
    FILE *duty_cycle_file = fopen(buf, "w");
    if (!duty_cycle_file) {
        perror("Failed to open PWM duty cycle");
        return 1;
    }
    fprintf(duty_cycle_file, "%d", duty_cycle_ns);
    fclose(duty_cycle_file);
    return 0;
}

// 读取PWM当前占空比，返回duty_cycle_ns，失败返回-1
int pwm_read_duty_cycle(const char *pwm_path) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s/pwm0/duty_cycle", pwm_path);
    FILE *duty_cycle_file = fopen(buf, "r");
    if (!duty_cycle_file) {
        perror("Failed to open PWM duty cycle for read");
        return -1;
    }
    int duty_cycle_ns = -1;
    if (fscanf(duty_cycle_file, "%d", &duty_cycle_ns) != 1) {
        perror("Failed to read PWM duty cycle value");
        fclose(duty_cycle_file);
        return -1;
    }
    fclose(duty_cycle_file);
    return duty_cycle_ns;
}

int pwm_enable(const char *pwm_path, int enable) {
    char buf[128];
    // 使能PWM
    snprintf(buf, sizeof(buf), "%s/pwm0/enable", pwm_path);
    FILE *enable_file = fopen(buf, "w");
    if (!enable_file) {
        perror("Failed to open PWM enable");
        return 1;
    }
    fprintf(enable_file, "%d", enable ? 1 : 0);
    fclose(enable_file);
    return 0;
}

int pwm_release(const char *pwm_path) {
    char buf[128];
    // 释放PWM
    snprintf(buf, sizeof(buf), "%s/unexport", pwm_path);
    FILE *pwm_unexport = fopen(buf, "w");
    if (!pwm_unexport) {
        perror("Failed to open PWM unexport");
        return 1;
    }
    fprintf(pwm_unexport, "0");
    fclose(pwm_unexport);
    return 0;
}

