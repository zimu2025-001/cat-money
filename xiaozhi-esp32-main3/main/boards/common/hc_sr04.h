#ifndef HC_SR04_H
#define HC_SR04_H

#include <driver/gpio.h>
#include <esp_err.h>
#include <cstdint>

/**
 * @brief HC-SR04 超声波测距传感器驱动类
 *
 * 工作电压: DC 5V
 * 工作电流: 15mA
 * 工作频率: 40kHz
 * 测距范围: 2cm - 400cm
 * 测量精度: 约 3mm
 * 测量角度: 15°
 *
 * 使用方法:
 * 1. 创建实例: HC_SR04 sensor(GPIO_NUM_4, GPIO_NUM_5);
 * 2. 初始化: sensor.Init();
 * 3. 读取距离: float distance = sensor.ReadDistance();
 */
class HC_SR04 {
public:
    /**
     * @brief 构造函数
     * @param trig_pin 触发引脚 (输出)
     * @param echo_pin 回波引脚 (输入)
     */
    HC_SR04(gpio_num_t trig_pin, gpio_num_t echo_pin);

    /**
     * @brief 初始化传感器
     * @return esp_err_t 错误代码
     */
    esp_err_t Init();

    /**
     * @brief 读取距离 (厘米)
     * @return float 距离值 (cm)，错误时返回 -1
     */
    float ReadDistance();

    /**
     * @brief 获取最后读取的距离值
     * @return float 距离值 (cm)
     */
    float GetLastDistance() const { return last_distance_; }

    /**
     * @brief 检查传感器是否已初始化
     * @return bool 是否已初始化
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * @brief 设置超时时间 (毫秒)
     * @param timeout_ms 超时时间
     */
    void SetTimeout(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

private:
    gpio_num_t trig_pin_;
    gpio_num_t echo_pin_;
    bool initialized_;
    float last_distance_;
    uint32_t timeout_ms_;  // 超时时间，默认 30ms (对应约 5 米)

    /**
     * @brief 发送触发信号
     */
    void SendTrigger();

    /**
     * @brief 测量回波高电平持续时间
     * @return uint32_t 高电平时间 (微秒)，超时返回 0
     */
    uint32_t MeasureEcho();
};

#endif // HC_SR04_H