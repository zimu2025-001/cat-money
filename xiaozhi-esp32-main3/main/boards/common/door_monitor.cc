#include "door_monitor.h"
#include "application.h"
#include "../../assets/lang_config.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>

#define TAG "DoorMonitor"

// 默认配置
#define DEFAULT_DETECTION_THRESHOLD   100.0f   // 默认检测距离阈值 100cm
#define DEFAULT_COOLDOWN_MS           5000     // 默认冷却时间 5 秒
#define DEFAULT_MOTION_DELTA_CM       5.0f
#define DEFAULT_NOISE_IGNORE_CM       2.0f
#define DEFAULT_MOTION_CONFIRM_COUNT  2
#define DEFAULT_PRESENCE_LOST_MS      1500
#define MONITOR_INTERVAL_MS           200      // 监控间隔 200ms
#define STABLE_DETECTION_COUNT        3        // 连续检测到次数，用于确认

DoorMonitor::DoorMonitor(gpio_num_t trig_pin, gpio_num_t echo_pin)
    : sensor_(trig_pin, echo_pin)
    , trig_pin_(trig_pin)
    , echo_pin_(echo_pin)
    , running_(false)
    , monitor_task_handle_(nullptr)
    , detection_threshold_(DEFAULT_DETECTION_THRESHOLD)
    , cooldown_ms_(DEFAULT_COOLDOWN_MS)
    , welcome_sound_enabled_(true)
    , detection_mode_(DetectionMode::Presence)
    , motion_delta_cm_(DEFAULT_MOTION_DELTA_CM)
    , noise_ignore_cm_(DEFAULT_NOISE_IGNORE_CM)
    , motion_confirm_count_(DEFAULT_MOTION_CONFIRM_COUNT)
    , presence_lost_ms_(DEFAULT_PRESENCE_LOST_MS)
    , active_presence_(false)
    , on_person_detected_(nullptr)
    , last_detection_time_(0)
    , last_presence_time_(0)
    , motion_baseline_distance_(-1.0f)
    , has_motion_baseline_(false)
    , had_clear_space_(false)
    , motion_count_(0) {
}

DoorMonitor::~DoorMonitor() {
    Stop();
}

esp_err_t DoorMonitor::Init() {
    esp_err_t ret = sensor_.Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HC-SR04: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Door monitor initialized - Trig: GPIO%d, Echo: GPIO%d", trig_pin_, echo_pin_);
    ESP_LOGI(TAG, "Detection threshold: %.1f cm, Cooldown: %lu ms",
             detection_threshold_, cooldown_ms_);

    return ESP_OK;
}

bool DoorMonitor::Start() {
    if (running_) {
        ESP_LOGW(TAG, "Monitor already running");
        return true;
    }

    if (!sensor_.IsInitialized()) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return false;
    }

    running_ = true;

    // 创建监控任务
    BaseType_t ret = xTaskCreate(
        MonitorTask,
        "door_monitor",
        4096,  // 堆栈大小
        this,
        5,     // 优先级
        &monitor_task_handle_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        running_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Door monitor started");
    return true;
}

void DoorMonitor::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (monitor_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS + 50));  // 等待任务结束
        monitor_task_handle_ = nullptr;
    }

    ESP_LOGI(TAG, "Door monitor stopped");
}

void DoorMonitor::SetMotionSensitivity(float motion_delta_cm, float noise_ignore_cm, int confirm_count) {
    motion_delta_cm_ = motion_delta_cm > 0 ? motion_delta_cm : DEFAULT_MOTION_DELTA_CM;
    noise_ignore_cm_ = noise_ignore_cm >= 0 ? noise_ignore_cm : DEFAULT_NOISE_IGNORE_CM;
    motion_confirm_count_ = confirm_count > 0 ? confirm_count : DEFAULT_MOTION_CONFIRM_COUNT;
}

void DoorMonitor::MonitorTask(void* arg) {
    DoorMonitor* self = static_cast<DoorMonitor*>(arg);
    self->ProcessMonitor();
    vTaskDelete(nullptr);
}

void DoorMonitor::ProcessMonitor() {
    ESP_LOGI(TAG, "Monitor task started - threshold: %.1f cm", detection_threshold_);

    int stable_count = 0;

    while (running_) {
        float distance = sensor_.ReadDistance();
        int64_t current_time = esp_timer_get_time() / 1000;

        if (detection_mode_ == DetectionMode::Motion) {
            ProcessMotionMode(distance, current_time);
        } else {
            ProcessPresenceMode(distance, current_time, stable_count);
        }

        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Monitor task ended");
}

void DoorMonitor::UpdatePresence(bool in_range, int64_t current_time) {
    if (in_range) {
        last_presence_time_ = current_time;
        active_presence_.store(true);
        return;
    }

    if (last_presence_time_ == 0 || (current_time - last_presence_time_) >= presence_lost_ms_) {
        active_presence_.store(false);
    }
}

void DoorMonitor::TriggerDetection(float distance, int64_t current_time) {
    if ((current_time - last_detection_time_) < cooldown_ms_) {
        return;
    }

    ESP_LOGI(TAG, "Person detected at %.1f cm!", distance);
    last_detection_time_ = current_time;

    if (on_person_detected_) {
        on_person_detected_(distance);
    }

    if (welcome_sound_enabled_) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_WELCOME);
        });
    }
}

void DoorMonitor::ProcessPresenceMode(float distance, int64_t current_time, int& stable_count) {
    const bool in_range = distance > 0 && distance < detection_threshold_;
    UpdatePresence(in_range, current_time);

    if (in_range) {
        stable_count++;
        ESP_LOGI(TAG, "Object detected at %.1f cm (count: %d/%d)",
                 distance, stable_count, STABLE_DETECTION_COUNT);

        if (stable_count >= STABLE_DETECTION_COUNT) {
            TriggerDetection(distance, current_time);
            stable_count = 0;
        }
    } else {
        stable_count = 0;
    }
}

void DoorMonitor::ProcessMotionMode(float distance, int64_t current_time) {
    const bool valid = distance > 0;
    const bool in_range = valid && distance < detection_threshold_;
    UpdatePresence(in_range, current_time);

    if (!valid) {
        motion_count_ = 0;
        has_motion_baseline_ = false;
        had_clear_space_ = true;
        return;
    }

    if (!has_motion_baseline_) {
        motion_baseline_distance_ = distance;
        has_motion_baseline_ = true;
        motion_count_ = 0;
        return;
    }

    if (!in_range) {
        motion_baseline_distance_ = distance;
        motion_count_ = 0;
        had_clear_space_ = true;
        return;
    }

    if (had_clear_space_) {
        motion_count_++;
        ESP_LOGI(TAG, "Entry motion detected at %.1f cm (count: %d/%d)",
                 distance, motion_count_, motion_confirm_count_);
        if (motion_count_ >= motion_confirm_count_) {
            TriggerDetection(distance, current_time);
            motion_count_ = 0;
            had_clear_space_ = false;
        }
        motion_baseline_distance_ = distance;
        return;
    }

    float delta = fabsf(distance - motion_baseline_distance_);
    if (delta >= motion_delta_cm_) {
        motion_count_++;
        ESP_LOGI(TAG, "Motion detected at %.1f cm, delta=%.1f cm (count: %d/%d)",
                 distance, delta, motion_count_, motion_confirm_count_);
        if (motion_count_ >= motion_confirm_count_) {
            TriggerDetection(distance, current_time);
            motion_baseline_distance_ = distance;
            motion_count_ = 0;
            had_clear_space_ = false;
        }
    } else {
        motion_count_ = 0;
        if (delta <= noise_ignore_cm_) {
            motion_baseline_distance_ = distance;
        }
    }
}
