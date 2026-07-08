/**
 * DHT11 温湿度传感器驱动
 * 完全重写，参考 C:\test 验证过的代码
 */

#ifndef DHT11_H
#define DHT11_H

#include <driver/gpio.h>
#include <esp_err.h>
#include <cstdint>

/**
 * DHT11 温湿度传感器驱动类
 * 使用验证过的时序逻辑
 */
class DHT11 {
public:
    /**
     * 构造函数
     * @param pin GPIO 引脚号
     * @param sensor_type 传感器类型（暂时只支持 DHT11）
     */
    DHT11(gpio_num_t pin, bool sensor_type = false);

    /**
     * 初始化传感器
     */
    esp_err_t Init();

    /**
     * 读取温湿度数据
     * @param temperature 输出：温度值(°C)
     * @param humidity 输出：湿度值(%)
     * @return ESP_OK 成功，ESP_ERR_TIMEOUT 失败
     */
    esp_err_t Read(float& temperature, float& humidity);

    /**
     * 获取最后读取的温度
     */
    float GetTemperature() const { return temperature_; }

    /**
     * 获取最后读取的湿度
     */
    float GetHumidity() const { return humidity_; }

private:
    gpio_num_t pin_;
    bool is_dht22_;
    float temperature_;
    float humidity_;

    // 内部辅助函数 - 直接使用 C:\test 的实现
    int ReadRaw(float* temp, float* humi);
};

#endif // DHT11_H