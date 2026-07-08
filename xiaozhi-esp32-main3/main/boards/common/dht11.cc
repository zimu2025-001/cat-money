/**
 * DHT11 温湿度传感器驱动
 * 完全移植自 C:\test 验证过的代码
 */

#include "dht11.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "DHT11"

// 微秒延时 - 完全使用 C:\test 的实现
static void delay_us(uint32_t us) {
    uint32_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < us);
}

DHT11::DHT11(gpio_num_t pin, bool sensor_type)
    : pin_(pin), is_dht22_(sensor_type), temperature_(0), humidity_(0) {
}

esp_err_t DHT11::Init() {
    // 完全按照 C:\test 的初始化方式
    gpio_reset_pin(pin_);
    gpio_set_direction(pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(pin_, 1);

    ESP_LOGI(TAG, "DHT11 initialized on GPIO %d", pin_);
    return ESP_OK;
}

// 完全移植 C:\test 的 dht11_read 函数
int DHT11::ReadRaw(float* temp, float* humi) {
    uint8_t data[5] = {0};
    uint8_t i, j;

    // 发送起始信号
    gpio_set_direction(pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // 拉低至少18ms
    gpio_set_level(pin_, 1);
    delay_us(30); // 拉高20-40us

    // 等待DHT11响应
    gpio_set_direction(pin_, GPIO_MODE_INPUT);
    uint32_t timeout = 0;

    // 等待低电平响应
    timeout = 0;
    while (gpio_get_level(pin_) && timeout < 100) {
        delay_us(1);
        timeout++;
    }
    if (timeout >= 100) return -1;

    // 等待高电平响应
    timeout = 0;
    while (!gpio_get_level(pin_) && timeout < 100) {
        delay_us(1);
        timeout++;
    }
    if (timeout >= 100) return -1;

    // 等待低电平开始传输数据
    timeout = 0;
    while (gpio_get_level(pin_) && timeout < 100) {
        delay_us(1);
        timeout++;
    }
    if (timeout >= 100) return -1;

    // 读取40位数据
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 8; j++) {
            // 等待高电平开始
            timeout = 0;
            while (!gpio_get_level(pin_) && timeout < 100) {
                delay_us(1);
                timeout++;
            }
            if (timeout >= 100) return -1;

            // 计算高电平持续时间
            uint32_t start = esp_timer_get_time();
            timeout = 0;
            while (gpio_get_level(pin_) && timeout < 100) {
                delay_us(1);
                timeout++;
            }
            uint32_t duration = esp_timer_get_time() - start;

            // 高电平持续时间>40us表示'1'，否则为'0'
            data[i] <<= 1;
            if (duration > 40) {
                data[i] |= 1;
            }
        }
    }

    // 校验和验证
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        ESP_LOGW(TAG, "Checksum error");
        return -1;
    }

    // DHT11数据格式：湿度整数 + 湿度小数 + 温度整数 + 温度小数 + 校验和
    *humi = (float)data[0];
    *temp = (float)data[2];

    return 0;
}

esp_err_t DHT11::Read(float& temperature, float& humidity) {
    ESP_LOGI(TAG, "Reading DHT11 on GPIO %d...", pin_);

    float temp, humi;
    int result = ReadRaw(&temp, &humi);

    if (result != 0) {
        ESP_LOGW(TAG, "DHT11 read failed on GPIO %d", pin_);
        return ESP_ERR_TIMEOUT;
    }

    temperature = temp;
    humidity = humi;
    temperature_ = temp;
    humidity_ = humi;

    ESP_LOGI(TAG, "DHT11: T=%.1f°C, H=%.1f%%", temperature, humidity);
    return ESP_OK;
}