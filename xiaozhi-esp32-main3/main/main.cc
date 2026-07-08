#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

// 舵机控制器初始化（如果启用）
#ifdef CONFIG_ENABLE_SG90_SERVO
extern "C" void InitializeSG90ServoController();
#endif

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SG90 servo controller if enabled
#ifdef CONFIG_ENABLE_SG90_SERVO
    ESP_LOGI(TAG, "Initializing SG90 Servo Controller...");
    InitializeSG90ServoController();
#endif

    // MQ135 smoke alarm and DHT11 sensor are initialized per-board.

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
