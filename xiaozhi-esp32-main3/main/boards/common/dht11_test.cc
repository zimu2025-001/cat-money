// DHT11 测试程序
// 在 main.cc 的 app_main 函数中添加测试代码，或创建独立测试

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#define TAG "DHT11_TEST"
#define DHT11_PIN GPIO_NUM_8

static inline void delay_us(uint32_t us) {
    if (us == 0) return;
    esp_rom_delay_us(us);
}

void dht11_test_task(void *pvParameters) {
    // 配置 GPIO
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << DHT11_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DHT11_PIN, 1);
    
    ESP_LOGI(TAG, "DHT11 test started on GPIO %d", DHT11_PIN);
    
    while (1) {
        uint8_t data[5] = {0};
        
        // 1. 主机起始信号
        gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(DHT11_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(18));  // 18ms
        gpio_set_level(DHT11_PIN, 1);
        delay_us(30);
        
        // 2. 切换到输入
        gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);
        gpio_pullup_en(DHT11_PIN);
        
        // 3. 等待传感器响应
        int timeout = 100;
        while (gpio_get_level(DHT11_PIN) == 1 && timeout > 0) {
            delay_us(1);
            timeout--;
        }
        if (timeout == 0) {
            ESP_LOGE(TAG, "No response from sensor (timeout low)");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ESP_LOGI(TAG, "Sensor response low OK, timeout=%d", timeout);
        
        timeout = 100;
        while (gpio_get_level(DHT11_PIN) == 0 && timeout > 0) {
            delay_us(1);
            timeout--;
        }
        if (timeout == 0) {
            ESP_LOGE(TAG, "No response from sensor (timeout high)");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ESP_LOGI(TAG, "Sensor response high OK, timeout=%d", timeout);
        
        // 4. 读取 40 位数据
        bool success = true;
        for (int i = 0; i < 40; i++) {
            timeout = 100;
            while (gpio_get_level(DHT11_PIN) == 0 && timeout > 0) {
                delay_us(1);
                timeout--;
            }
            if (timeout == 0) {
                ESP_LOGE(TAG, "Timeout reading bit %d", i);
                success = false;
                break;
            }
            
            uint32_t high_time = 0;
            while (gpio_get_level(DHT11_PIN) == 1 && high_time < 100) {
                high_time++;
                delay_us(1);
            }
            
            data[i / 8] <<= 1;
            if (high_time > 40) {
                data[i / 8] |= 1;
            }
        }
        
        if (success) {
            // 验证校验和
            uint8_t checksum = data[0] + data[1] + data[2] + data[3];
            ESP_LOGI(TAG, "Data: %02x %02x %02x %02x %02x", 
                     data[0], data[1], data[2], data[3], data[4]);
            ESP_LOGI(TAG, "Checksum calc: 0x%02x, received: 0x%02x", 
                     checksum, data[4]);
            
            if (checksum == data[4]) {
                float humidity = data[0] + data[1] * 0.01f;
                float temp = data[2] + data[3] * 0.01f;
                ESP_LOGI(TAG, "Temperature: %.1f°C, Humidity: %.1f%%", temp, humidity);
            } else {
                ESP_LOGE(TAG, "Checksum mismatch!");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// 在 app_main 中调用
// vTaskDelay(pdMS_TO_TICKS(5000));  // 等待系统启动
// xTaskCreate(dht11_test_task, "dht11_test", 4096, NULL, 5, NULL);
