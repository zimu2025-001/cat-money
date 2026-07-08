#ifndef SG90_SERVO_H
#define SG90_SERVO_H

#include <algorithm>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_err.h>
#include <esp_log.h>

class SG90Servo {
public:
    SG90Servo()
        : gpio_pin_(GPIO_NUM_NC),
          ledc_channel_(LEDC_CHANNEL_0),
          ledc_timer_(LEDC_TIMER_0),
          ledc_speed_mode_(LEDC_LOW_SPEED_MODE),
          initialized_(false),
          current_angle_(90) {}

    ~SG90Servo() {
        if (initialized_) {
            ledc_stop(ledc_speed_mode_, ledc_channel_, 0);
        }
    }

    bool Init(gpio_num_t gpio,
              ledc_timer_t timer,
              ledc_channel_t channel,
              int initial_angle = 90) {
        if (initialized_) {
            ESP_LOGW(TAG, "servo on GPIO%d already initialized", gpio_pin_);
            return false;
        }

        gpio_pin_ = gpio;
        ledc_channel_ = channel;
        ledc_timer_ = timer;
        ledc_speed_mode_ = LEDC_LOW_SPEED_MODE;

        ledc_timer_config_t timer_conf = {};
        timer_conf.speed_mode = ledc_speed_mode_;
        timer_conf.duty_resolution = LEDC_TIMER_13_BIT;
        timer_conf.timer_num = ledc_timer_;
        timer_conf.freq_hz = 50;
        timer_conf.clk_cfg = LEDC_AUTO_CLK;
        timer_conf.deconfigure = false;

        esp_err_t ret = ledc_timer_config(&timer_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to configure LEDC timer: %s", esp_err_to_name(ret));
            return false;
        }

        ledc_channel_config_t ch_conf = {};
        ch_conf.speed_mode = ledc_speed_mode_;
        ch_conf.channel = ledc_channel_;
        ch_conf.timer_sel = ledc_timer_;
        ch_conf.intr_type = LEDC_INTR_DISABLE;
        ch_conf.gpio_num = gpio_pin_;
        ch_conf.duty = 0;
        ch_conf.hpoint = 0;
        ch_conf.sleep_mode = (ledc_sleep_mode_t)0;

        ret = ledc_channel_config(&ch_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to configure LEDC channel: %s", esp_err_to_name(ret));
            return false;
        }

        initialized_ = true;
        SetAngle(initial_angle);
        ESP_LOGI(TAG, "servo initialized on GPIO%d, channel=%d, angle=%d", gpio_pin_, ledc_channel_, current_angle_);
        return true;
    }

    bool SetAngle(int angle) {
        if (!initialized_) {
            ESP_LOGE(TAG, "servo not initialized");
            return false;
        }
        current_angle_ = ClampAngle(angle);
        WriteDuty(AngleToDuty(current_angle_));
        return true;
    }

    bool MoveBy(int delta) {
        return SetAngle(current_angle_ + delta);
    }

    int GetAngle() const {
        return current_angle_;
    }

    bool IsInitialized() const {
        return initialized_;
    }

private:
    static constexpr int kMinPulsewidthUs = 500;
    static constexpr int kMaxPulsewidthUs = 2500;
    static constexpr int kPeriodUs = 20000;
    inline static constexpr const char* TAG = "SG90Servo";

    gpio_num_t gpio_pin_;
    ledc_channel_t ledc_channel_;
    ledc_timer_t ledc_timer_;
    ledc_mode_t ledc_speed_mode_;
    bool initialized_;
    int current_angle_;

    static int ClampAngle(int angle) {
        return std::max(0, std::min(180, angle));
    }

    static uint32_t AngleToDuty(int angle) {
        const uint32_t pulse_width = kMinPulsewidthUs +
            (angle * (kMaxPulsewidthUs - kMinPulsewidthUs)) / 180;
        return (pulse_width * 8192) / kPeriodUs;
    }

    void WriteDuty(uint32_t duty) {
        ledc_set_duty_with_hpoint(ledc_speed_mode_, ledc_channel_, duty, 0);
        ledc_update_duty(ledc_speed_mode_, ledc_channel_);
    }
};

#endif  // SG90_SERVO_H
