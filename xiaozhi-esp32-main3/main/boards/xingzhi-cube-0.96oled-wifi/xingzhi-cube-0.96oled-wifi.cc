#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "custom_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_save_timer.h"
#include "../xingzhi-cube-1.54tft-wifi/power_manager.h"
#include "boards/common/dht11.h"
#include "boards/common/door_monitor.h"
#include "boards/common/mq135.h"

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>

#define TAG "XINGZHI_CUBE_TFT_WIFI"

class XINGZHI_CUBE_TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button screen_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    CustomDisplay* display_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    DHT11* dht11_ = nullptr;
    DoorMonitor* door_monitor_ = nullptr;
    Mq135Sensor* mq135_ = nullptr;
    int screen_index_ = 0;  // 0=main, 1=info

    void SwitchScreen() {
        screen_index_ = (screen_index_ + 1) % 2;
        if (display_) {
            display_->RequestSwitchToNextScreen();
        }
        ESP_LOGI(TAG, "Screen switched to %d", screen_index_);
    }

    static void Dht11Task(void* arg) {
        XINGZHI_CUBE_TFT_WIFI* board = static_cast<XINGZHI_CUBE_TFT_WIFI*>(arg);
        vTaskDelay(pdMS_TO_TICKS(2000));

        while (true) {
            if (board->dht11_ && board->display_) {
                float temperature, humidity;
                esp_err_t ret = board->dht11_->Read(temperature, humidity);
                if (ret == ESP_OK) {
                    board->display_->UpdateTemperature(temperature, humidity);
                    ESP_LOGI(TAG, "DHT11: T=%.1f°C, H=%.1f%%", temperature, humidity);
                } else {
                    ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(ret));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    void InitializeMq135() {
        ESP_LOGI(TAG, "Initializing MQ135 on ADC%d CH%d", MQ135_ADC_UNIT, MQ135_ADC_CHAN);
        mq135_ = new Mq135Sensor(MQ135_ADC_UNIT, MQ135_ADC_CHAN);
        mq135_->StartPeriodicReading(3000, [this](float ppm) {
            const char* quality = Mq135Sensor::GetAirQualityLevel(ppm);
            ESP_LOGI(TAG, "MQ135: %.1f PPM, %s", ppm, quality);
            if (display_) {
                display_->UpdateAirQuality(ppm, quality);
            }
        });
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_38);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (!power_save_timer_) {
                return;
            }
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_21);
        rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_21, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(GPIO_NUM_21, 0);
            rtc_gpio_hold_en(GPIO_NUM_21);
            esp_lcd_panel_disp_on_off(panel_, false);
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7789 LCD driver, width=%d, height=%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));

        ESP_LOGI(TAG, "Reset display");
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        ESP_LOGI(TAG, "Display initialized");
        display_ = new CustomDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            SwitchScreen();
        });

        boot_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            EnterWifiConfigMode();
        });

        screen_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            SwitchScreen();
        });

        screen_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            EnterWifiConfigMode();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeDoorMonitor() {
        ESP_LOGI(TAG, "Initializing door monitor with HC-SR04...");
        door_monitor_ = new DoorMonitor(HC_SR04_TRIG_PIN, HC_SR04_ECHO_PIN);

        esp_err_t ret = door_monitor_->Init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize door monitor: %s", esp_err_to_name(ret));
            delete door_monitor_;
            door_monitor_ = nullptr;
            return;
        }

        door_monitor_->SetDetectionThreshold(DOOR_MONITOR_DETECTION_THRESHOLD_CM);
        door_monitor_->SetCooldownTime(DOOR_MONITOR_COOLDOWN_MS);

        door_monitor_->SetOnPersonDetectedCallback([this](float distance) {
            ESP_LOGI(TAG, "Person detected at %.1f cm!", distance);
            power_save_timer_->WakeUp();
        });

        door_monitor_->Start();
        ESP_LOGI(TAG, "Door monitor started successfully");
    }

public:
    XINGZHI_CUBE_TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        screen_button_(SCREEN_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        door_monitor_(nullptr) {
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        InitializeDoorMonitor();
        InitializeMq135();
        GetBacklight()->RestoreBrightness();

        // Initialize DHT11
        ESP_LOGI(TAG, "Initializing DHT11 sensor on GPIO %d", DHT11_PIN);
        dht11_ = new DHT11(DHT11_PIN, DHT11_TYPE_DHT22);
        esp_err_t ret = dht11_->Init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize DHT11: %s", esp_err_to_name(ret));
            delete dht11_;
            dht11_ = nullptr;
        } else {
            ESP_LOGI(TAG, "DHT11 sensor initialized successfully");
            xTaskCreate(Dht11Task, "dht11_task", 4096, this, 1, nullptr);
            ESP_LOGI(TAG, "DHT11 task started");
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        level = 100;
        charging = false;
        discharging = false;
        return true;
    }

    virtual DHT11* GetDHT11() override {
        return dht11_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XINGZHI_CUBE_TFT_WIFI);
