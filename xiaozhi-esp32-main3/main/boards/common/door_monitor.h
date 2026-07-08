#ifndef DOOR_MONITOR_H
#define DOOR_MONITOR_H

#include "hc_sr04.h"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <functional>
#include <string>

/**
 * @brief 门口监控模块 - 使用超声波传感器检测有人进入
 *
 * 功能：
 * - 持续监测门口距离
 * - 检测到有人进入时触发回调，可选播放欢迎语音
 * - 支持静态存在检测或动态移动检测
 * - 支持配置检测距离阈值和冷却时间
 *
 * 使用方法：
 * 1. 创建实例: DoorMonitor monitor(trig_pin, echo_pin);
 * 2. 初始化: monitor.Init();
 * 3. 启动监控: monitor.Start();
 * 4. 停止监控: monitor.Stop();
 */
class DoorMonitor {
public:
    enum class DetectionMode {
        Presence,
        Motion,
    };

    /**
     * @brief 检测到有人进入时的回调类型
     * @param distance 检测到的距离 (cm)
     */
    using OnPersonDetectedCallback = std::function<void(float distance)>;

    /**
     * @brief 构造函数
     * @param trig_pin 超声波传感器触发引脚
     * @param echo_pin 超声波传感器回波引脚
     */
    DoorMonitor(gpio_num_t trig_pin, gpio_num_t echo_pin);

    ~DoorMonitor();

    /**
     * @brief 初始化监控模块
     * @return esp_err_t 错误代码
     */
    esp_err_t Init();

    /**
     * @brief 启动监控
     * @return bool 是否成功启动
     */
    bool Start();

    /**
     * @brief 停止监控
     */
    void Stop();

    /**
     * @brief 设置检测距离阈值 (厘米)
     * @param threshold 距离阈值，小于此距离认为有人
     */
    void SetDetectionThreshold(float threshold) { detection_threshold_ = threshold; }

    /**
     * @brief 获取检测距离阈值
     * @return float 距离阈值 (cm)
     */
    float GetDetectionThreshold() const { return detection_threshold_; }

    /**
     * @brief 设置冷却时间 (毫秒)
     * @param cooldown_ms 两次检测之间的最小间隔
     */
    void SetCooldownTime(uint32_t cooldown_ms) { cooldown_ms_ = cooldown_ms; }

    void SetDetectionMode(DetectionMode mode) { detection_mode_ = mode; }
    void SetMotionSensitivity(float motion_delta_cm, float noise_ignore_cm, int confirm_count);
    void SetPresenceLostTime(uint32_t lost_ms) { presence_lost_ms_ = lost_ms; }
    bool HasActivePresence() const { return active_presence_.load(); }

    /**
     * @brief 设置检测到有人时的回调
     * @param callback 回调函数
     */
    void SetOnPersonDetectedCallback(OnPersonDetectedCallback callback) {
        on_person_detected_ = callback;
    }

    /**
     * @brief 启用/禁用欢迎语音
     * @param enable 是否启用
     */
    void SetWelcomeSoundEnabled(bool enable) { welcome_sound_enabled_ = enable; }

    /**
     * @brief 检查监控是否正在运行
     * @return bool 是否正在运行
     */
    bool IsRunning() const { return running_; }

    /**
     * @brief 获取超声波传感器实例
     * @return HC_SR04* 传感器指针
     */
    HC_SR04* GetSensor() { return &sensor_; }

private:
    HC_SR04 sensor_;
    gpio_num_t trig_pin_;
    gpio_num_t echo_pin_;

    bool running_;
    TaskHandle_t monitor_task_handle_;

    // 配置参数
    float detection_threshold_;      // 检测距离阈值 (cm)
    uint32_t cooldown_ms_;           // 冷却时间 (ms)
    bool welcome_sound_enabled_;      // 是否播放欢迎语音
    DetectionMode detection_mode_;    // 检测模式
    float motion_delta_cm_;           // 有效移动阈值
    float noise_ignore_cm_;           // 噪声忽略阈值
    int motion_confirm_count_;        // 动态确认次数
    uint32_t presence_lost_ms_;       // 人员离开判定时间
    std::atomic<bool> active_presence_;

    // 回调
    OnPersonDetectedCallback on_person_detected_;

    // 上次检测时间
    int64_t last_detection_time_;
    int64_t last_presence_time_;
    float motion_baseline_distance_;
    bool has_motion_baseline_;
    bool had_clear_space_;
    int motion_count_;

    // 监控任务
    static void MonitorTask(void* arg);
    void ProcessMonitor();
    void TriggerDetection(float distance, int64_t current_time);
    void ProcessPresenceMode(float distance, int64_t current_time, int& stable_count);
    void ProcessMotionMode(float distance, int64_t current_time);
    void UpdatePresence(bool in_range, int64_t current_time);
};

#endif // DOOR_MONITOR_H
