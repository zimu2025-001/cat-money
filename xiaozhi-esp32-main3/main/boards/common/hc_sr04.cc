#include "hc_sr04.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#define TAG "HC_SR04"

// 声速 (cm/us)，温度 20°C 时的声速约 343m/s
#define SOUND_SPEED_CM_PER_US  0.0343f

// 触发信号持续时间 (微秒)
#define TRIGGER_PULSE_US       10

// 默认超时时间 (微秒) - 30ms 对应约 5 米
#define DEFAULT_TIMEOUT_US     30000

// 最小测量距离 (cm)
#define MIN_DISTANCE           2.0f

// 最大测量距离 (cm)
#define MAX_DISTANCE           400.0f

HC_SR04::HC_SR04(gpio_num_t trig_pin, gpio_num_t echo_pin)
    : trig_pin_(trig_pin)
    , echo_pin_(echo_pin)
    , initialized_(false)
    , last_distance_(-1.0f)
    , timeout_ms_(30) {
}

esp_err_t HC_SR04::Init() {
    // 配置 Trig 引脚为输出
    gpio_config_t trig_conf = {};
    trig_conf.pin_bit_mask = (1ULL << trig_pin_);
    trig_conf.mode = GPIO_MODE_OUTPUT;
    trig_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    trig_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    trig_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&trig_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Trig GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始输出低电平
    gpio_set_level(trig_pin_, 0);

    // 配置 Echo 引脚为输入
    gpio_config_t echo_conf = {};
    echo_conf.pin_bit_mask = (1ULL << echo_pin_);
    echo_conf.mode = GPIO_MODE_INPUT;
    echo_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    echo_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;  // 下拉，防止悬空
    echo_conf.intr_type = GPIO_INTR_DISABLE;

    ret = gpio_config(&echo_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echo GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "HC-SR04 initialized - Trig: GPIO%d, Echo: GPIO%d", trig_pin_, echo_pin_);

    return ESP_OK;
}

void HC_SR04::SendTrigger() {
    // 确保 Trig 为低电平
    gpio_set_level(trig_pin_, 0);
    esp_rom_delay_us(2);

    // 发送至少 10us 的高电平脉冲
    gpio_set_level(trig_pin_, 1);
    esp_rom_delay_us(TRIGGER_PULSE_US);

    // 拉低
    gpio_set_level(trig_pin_, 0);
}

uint32_t HC_SR04::MeasureEcho() {
    uint32_t timeout_us = timeout_ms_ * 1000;

    // 等待 Echo 变高 (超时保护)
    uint32_t start_time = esp_timer_get_time();
    while (gpio_get_level(echo_pin_) == 0) {
        if ((esp_timer_get_time() - start_time) > timeout_us) {
            ESP_LOGD(TAG, "Echo timeout (waiting for high)");
            return 0;
        }
    }

    // 记录高电平开始时间
    uint32_t echo_start = esp_timer_get_time();

    // 等待 Echo 变低 (测量高电平持续时间)
    while (gpio_get_level(echo_pin_) == 1) {
        if ((esp_timer_get_time() - echo_start) > timeout_us) {
            ESP_LOGD(TAG, "Echo timeout (waiting for low)");
            return 0;
        }
    }

    // 计算高电平持续时间
    uint32_t echo_duration = esp_timer_get_time() - echo_start;

    return echo_duration;
}

float HC_SR04::ReadDistance() {
    if (!initialized_) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return -1.0f;
    }

    // 发送触发信号
    SendTrigger();

    // 测量回波
    uint32_t echo_time = MeasureEcho();

    if (echo_time == 0) {
        last_distance_ = -1.0f;
        return -1.0f;
    }

    // 计算距离: 距离 = (时间 × 声速) / 2
    // 时间单位是微秒，声速约 0.0343 cm/us
    float distance = (echo_time * SOUND_SPEED_CM_PER_US) / 2.0f;

    // 检查有效范围
    if (distance < MIN_DISTANCE || distance > MAX_DISTANCE) {
        last_distance_ = -1.0f;
        return -1.0f;
    }

    last_distance_ = distance;

    return distance;
}