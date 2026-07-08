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
#ifdef CONFIG_ENABLE_LUCKY_CAT_SERVO
#include "lucky_cat_servo_controller.h"
#endif

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <atomic>

#define TAG "ZHIMU_2_25TFT_WIFI"

namespace {
constexpr uint16_t kScreenButtonLongPressMs = 1500;
constexpr uint16_t kScreenButtonDoubleClickWindowMs = 450;
constexpr const char* kDoorWelcomePrompt = "只说欢迎光临。";
}  // namespace

class ZHIMU_2_25TFT_WIFI : public WifiBoard {
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
    bool smoke_alarm_active_ = false;
    int64_t last_smoke_alarm_ms_ = 0;
    std::atomic<int64_t> last_door_wake_ms_ = 0;
    std::atomic<bool> door_auto_idle_task_running_ = false;
    std::atomic<bool> door_welcome_blocked_ = false;
    std::atomic<bool> door_rearm_task_running_ = false;
    int screen_index_ = 0;  // 0=main, 1=info

    void SwitchScreen() {
        screen_index_ = (screen_index_ + 1) % 2;
        if (display_) {
            display_->RequestSwitchToNextScreen();
        }
        ESP_LOGI(TAG, "Screen switched to %d", screen_index_);
    }

    static void Dht11Task(void* arg) {
        ZHIMU_2_25TFT_WIFI* board = static_cast<ZHIMU_2_25TFT_WIFI*>(arg);
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

    int CalculateSmokeLevel(int raw) const {
#if SMOKE_ALARM_HIGHER_RAW_IS_DANGER
        const int denominator = SMOKE_ALARM_ADC_THRESHOLD > 0 ? SMOKE_ALARM_ADC_THRESHOLD : 1;
        int level = raw * 100 / denominator;
#else
        const int denominator = (4095 - SMOKE_ALARM_ADC_THRESHOLD) > 0 ? (4095 - SMOKE_ALARM_ADC_THRESHOLD) : 1;
        int level = (4095 - raw) * 100 / denominator;
#endif
        if (level < 0) {
            level = 0;
        } else if (level > 100) {
            level = 100;
        }
        return level;
    }

    bool IsSmokeAlarmRaw(int raw) const {
#if SMOKE_ALARM_HIGHER_RAW_IS_DANGER
        int clear_threshold = SMOKE_ALARM_ADC_THRESHOLD - SMOKE_ALARM_ADC_HYSTERESIS;
        if (clear_threshold < 0) clear_threshold = 0;
        return smoke_alarm_active_ ? raw > clear_threshold : raw >= SMOKE_ALARM_ADC_THRESHOLD;
#else
        int clear_threshold = SMOKE_ALARM_ADC_THRESHOLD + SMOKE_ALARM_ADC_HYSTERESIS;
        if (clear_threshold > 4095) clear_threshold = 4095;
        return smoke_alarm_active_ ? raw < clear_threshold : raw <= SMOKE_ALARM_ADC_THRESHOLD;
#endif
    }

    void HandleSmokeReading(int raw) {
        const int level = CalculateSmokeLevel(raw);
        const bool alarm = IsSmokeAlarmRaw(raw);
        ESP_LOGI(TAG, "Smoke sensor: raw=%d, level=%d%%, alarm=%s", raw, level, alarm ? "yes" : "no");

        if (display_) {
            display_->UpdateSmokeStatus(level, alarm);
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (alarm) {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }

            const bool should_alert = !smoke_alarm_active_ ||
                (now_ms - last_smoke_alarm_ms_) >= SMOKE_ALARM_REPEAT_MS;
            if (should_alert) {
                last_smoke_alarm_ms_ = now_ms;
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().Alert(
                        "烟雾警报",
                        "检测到烟雾，请立即检查店内环境",
                        "triangle_exclamation",
                        Lang::Sounds::OGG_EXCLAMATION);
                });
            }
        } else if (smoke_alarm_active_) {
            Application::GetInstance().Schedule([this]() {
                if (display_) {
                    display_->ShowNotification("烟雾恢复正常");
                }
            });
        }

        smoke_alarm_active_ = alarm;
    }

    void TriggerSmokeAlarmTest() {
        ESP_LOGW(TAG, "Manual smoke alarm test, injecting raw=%d", SMOKE_ALARM_TEST_RAW);
        if (display_) {
            display_->ShowNotification("烟雾测试");
        }
        HandleSmokeReading(SMOKE_ALARM_TEST_RAW);
    }

    static void SmokeAlarmTask(void* arg) {
        ZHIMU_2_25TFT_WIFI* board = static_cast<ZHIMU_2_25TFT_WIFI*>(arg);
        vTaskDelay(pdMS_TO_TICKS(1500));

#if SMOKE_ALARM_TEST_ON_BOOT
        vTaskDelay(pdMS_TO_TICKS(SMOKE_ALARM_TEST_DELAY_MS));
        ESP_LOGW(TAG, "Smoke alarm test mode enabled, injecting raw=%d", SMOKE_ALARM_TEST_RAW);
        board->HandleSmokeReading(SMOKE_ALARM_TEST_RAW);
#endif

        while (true) {
            if (board->mq135_) {
                const int raw = board->mq135_->GetRawValue();
                board->HandleSmokeReading(raw);
            }
            vTaskDelay(pdMS_TO_TICKS(SMOKE_ALARM_READ_INTERVAL_MS));
        }
    }

    void InitializeSmokeAlarm() {
        ESP_LOGI(TAG, "Initializing MQ135 smoke alarm on ADC%d CH%d", MQ135_ADC_UNIT, MQ135_ADC_CHAN);
        mq135_ = new Mq135Sensor(MQ135_ADC_UNIT, MQ135_ADC_CHAN);
        xTaskCreate(SmokeAlarmTask, "smoke_alarm_task", 4096, this, 2, nullptr);
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

        boot_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            TriggerSmokeAlarmTest();
        });

        screen_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            SwitchScreen();
        });

        screen_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            EnterWifiConfigMode();
        });

#ifdef CONFIG_ENABLE_LUCKY_CAT_SERVO
        screen_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            const bool enabled = LuckyCatServoToggleSwing();
            ESP_LOGI(TAG, "Screen button double click: lucky-cat swing %s", enabled ? "on" : "off");
            if (display_) {
                display_->ShowNotification(enabled ? "Swing ON" : "Swing OFF");
            }
        });
#endif

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

    static void DoorAutoIdleTask(void* arg) {
        ZHIMU_2_25TFT_WIFI* board = static_cast<ZHIMU_2_25TFT_WIFI*>(arg);

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(DOOR_MONITOR_AUTO_IDLE_CHECK_MS));

            auto& app = Application::GetInstance();
            if (app.IsVoiceDetected()) {
                ESP_LOGI(TAG, "Door auto-idle canceled: voice detected");
                break;
            }

            const int64_t last_wake_ms = board->last_door_wake_ms_.load();
            if (last_wake_ms <= 0) {
                break;
            }

            const int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - last_wake_ms) < DOOR_MONITOR_AUTO_IDLE_MS) {
                continue;
            }

            const bool has_person = board->door_monitor_ && board->door_monitor_->HasActivePresence();
            if (has_person) {
                continue;
            }

            const auto state = app.GetDeviceState();
            if (state == kDeviceStateSpeaking) {
                continue;
            }
            if (state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Door auto-idle: no voice and no person, stop listening");
                app.StopListening();
            }
            break;
        }

        board->door_auto_idle_task_running_.store(false);
        vTaskDelete(nullptr);
    }

    void StartDoorAutoIdleCheck() {
        last_door_wake_ms_.store(esp_timer_get_time() / 1000);
        bool expected = false;
        if (!door_auto_idle_task_running_.compare_exchange_strong(expected, true)) {
            return;
        }

        BaseType_t ret = xTaskCreate(DoorAutoIdleTask, "door_auto_idle", 4096, this, 3, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create door auto-idle task");
            door_auto_idle_task_running_.store(false);
        }
    }

    static void DoorWelcomeRearmTask(void* arg) {
        ZHIMU_2_25TFT_WIFI* board = static_cast<ZHIMU_2_25TFT_WIFI*>(arg);
        int64_t ready_since_ms = 0;

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(DOOR_MONITOR_REARM_CHECK_MS));

            auto& app = Application::GetInstance();
            const bool app_idle = app.GetDeviceState() == kDeviceStateIdle;
            const bool door_clear = !board->door_monitor_ || !board->door_monitor_->HasActivePresence();
            const int64_t now_ms = esp_timer_get_time() / 1000;

            if (app_idle && door_clear) {
                if (ready_since_ms == 0) {
                    ready_since_ms = now_ms;
                } else if ((now_ms - ready_since_ms) >= DOOR_MONITOR_REARM_IDLE_MS) {
                    break;
                }
            } else {
                ready_since_ms = 0;
            }
        }

        board->door_welcome_blocked_.store(false);
        board->door_rearm_task_running_.store(false);
        ESP_LOGI(TAG, "Door welcome rearmed after idle");
        vTaskDelete(nullptr);
    }

    void StartDoorWelcomeRearmTask() {
        bool expected = false;
        if (!door_rearm_task_running_.compare_exchange_strong(expected, true)) {
            return;
        }

        BaseType_t ret = xTaskCreate(DoorWelcomeRearmTask, "door_rearm", 4096, this, 3, nullptr);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create door welcome rearm task");
            door_rearm_task_running_.store(false);
            door_welcome_blocked_.store(false);
        }
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
        door_monitor_->SetDetectionMode(DoorMonitor::DetectionMode::Motion);
        door_monitor_->SetMotionSensitivity(
            DOOR_MONITOR_MOTION_DELTA_CM,
            DOOR_MONITOR_NOISE_IGNORE_CM,
            DOOR_MONITOR_MOTION_CONFIRM_COUNT);
        door_monitor_->SetPresenceLostTime(DOOR_MONITOR_PRESENCE_LOST_MS);
        door_monitor_->SetWelcomeSoundEnabled(false);

        door_monitor_->SetOnPersonDetectedCallback([this](float distance) {
            if (door_welcome_blocked_.load()) {
                ESP_LOGI(TAG, "Moving person detected at %.1f cm, ignored while welcome is blocked", distance);
                return;
            }

            auto& app = Application::GetInstance();
            const auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle) {
                ESP_LOGI(TAG, "Moving person detected at %.1f cm, ignored while state=%d",
                         distance, static_cast<int>(state));
                return;
            }

            bool expected = false;
            if (!door_welcome_blocked_.compare_exchange_strong(expected, true)) {
                return;
            }
            StartDoorWelcomeRearmTask();

            ESP_LOGI(TAG, "Moving person detected at %.1f cm, invoking XiaoZhi welcome prompt", distance);
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (display_) {
                display_->ShowNotification("迎宾触发");
            }
            StartDoorAutoIdleCheck();
            app.WakeWordInvoke(kDoorWelcomePrompt);
        });

        door_monitor_->Start();
        ESP_LOGI(TAG, "Door monitor started successfully");
    }

public:
    ZHIMU_2_25TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        screen_button_(SCREEN_BUTTON_GPIO, false, kScreenButtonLongPressMs, kScreenButtonDoubleClickWindowMs),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        door_monitor_(nullptr) {
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
#ifdef CONFIG_ENABLE_LUCKY_CAT_SERVO
        InitializeLuckyCatServoController();
#endif
        InitializeDoorMonitor();
        InitializeSmokeAlarm();
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

DECLARE_BOARD(ZHIMU_2_25TFT_WIFI);
