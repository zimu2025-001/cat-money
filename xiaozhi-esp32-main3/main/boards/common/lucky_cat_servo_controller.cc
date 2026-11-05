#include "lucky_cat_servo_controller.h"
#include "lucky_cat_remote_config.h"

#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#include "sg90_servo.h"
#include "system_info.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <web_socket.h>

#define TAG "LuckyCatServo"

#ifndef CONFIG_LUCKY_CAT_MG996R_SERVO_GPIO
#define CONFIG_LUCKY_CAT_MG996R_SERVO_GPIO 20
#endif

#ifndef CONFIG_LUCKY_CAT_SG90_SERVO_GPIO
#define CONFIG_LUCKY_CAT_SG90_SERVO_GPIO 11
#endif

#ifndef CONFIG_LUCKY_CAT_REMOTE_WS_URL
#define CONFIG_LUCKY_CAT_REMOTE_WS_URL ""
#endif

namespace {

constexpr int kDefaultStep = 5;
constexpr int kMaxStep = 45;
constexpr int kDefaultAngle = 90;
constexpr int kMg996rMinAngle = 20;
constexpr int kMg996rMaxAngle = 160;
constexpr int kSg90MinAngle = 30;
constexpr int kSg90MaxAngle = 150;
constexpr int kSwingMinAngle = 70;
constexpr int kSwingMaxAngle = 120;
constexpr int kSwingStep = 1;
constexpr int kSwingIntervalMs = 28;
constexpr int kSwingEndpointEaseDegrees = 8;
constexpr int kSwingEndpointIntervalMs = 55;
constexpr int kRemoteInitialRetryMs = 1000;
constexpr int kRemoteMaxRetryMs = 30000;

gpio_num_t Mg996rGpio() {
    return static_cast<gpio_num_t>(CONFIG_LUCKY_CAT_MG996R_SERVO_GPIO);
}

gpio_num_t Sg90Gpio() {
    return static_cast<gpio_num_t>(CONFIG_LUCKY_CAT_SG90_SERVO_GPIO);
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

class LuckyCatServoController {
public:
    LuckyCatServoController()
        : mutex_(xSemaphoreCreateMutex()) {
        initialized_ = mg996r_.Init(Mg996rGpio(), LEDC_TIMER_1, LEDC_CHANNEL_2, mg996r_angle_) &&
                       sg90_.Init(Sg90Gpio(), LEDC_TIMER_1, LEDC_CHANNEL_3, sg90_angle_);
        RegisterMcpTools();
        StartSwingTask();
        StartRemoteClient();
        ESP_LOGI(TAG, "initialized=%d, mg996r_gpio=%d, sg90_gpio=%d", initialized_, Mg996rGpio(), Sg90Gpio());
    }

    bool ToggleSwing() {
        Lock();
        swing_enabled_ = !swing_enabled_;
        if (swing_enabled_) {
            sg90_angle_ = ClampAngle(sg90_angle_, kSwingMinAngle, kSwingMaxAngle);
            sg90_.SetAngle(sg90_angle_);
            swing_direction_ = 1;
        }
        const bool enabled = swing_enabled_;
        Unlock();
        ESP_LOGI(TAG, "swing %s", enabled ? "enabled" : "disabled");
        return enabled;
    }

    bool IsSwingEnabled() {
        Lock();
        const bool enabled = swing_enabled_;
        Unlock();
        return enabled;
    }

    void StartRemoteClient() {
        const std::string url = GetRemoteUrl();
        if (url.empty()) {
            ESP_LOGI(TAG, "remote WebSocket disabled");
            return;
        }
        if (remote_task_handle_ != nullptr) {
            remote_should_run_ = true;
            return;
        }
        remote_should_run_ = true;
        xTaskCreate(RemoteTaskEntry, "lucky_cat_ws", 6144, this, 4, &remote_task_handle_);
    }

private:
    SG90Servo mg996r_;
    SG90Servo sg90_;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t swing_task_handle_ = nullptr;
    TaskHandle_t remote_task_handle_ = nullptr;
    bool initialized_ = false;
    bool swing_enabled_ = false;
    bool remote_should_run_ = false;
    int swing_direction_ = 1;
    int mg996r_angle_ = kDefaultAngle;
    int sg90_angle_ = kDefaultAngle;

    static void SwingTaskEntry(void* arg) {
        static_cast<LuckyCatServoController*>(arg)->SwingTask();
    }

    static void RemoteTaskEntry(void* arg) {
        static_cast<LuckyCatServoController*>(arg)->RemoteTask();
    }

    void StartSwingTask() {
        if (swing_task_handle_ == nullptr) {
            xTaskCreate(SwingTaskEntry, "lucky_cat_swing", 3072, this, 3, &swing_task_handle_);
        }
    }

    void SwingTask() {
        while (true) {
            bool active = false;
            int delay_ms = 100;
            Lock();
            active = initialized_ && swing_enabled_;
            if (active) {
                if (sg90_angle_ >= kSwingMaxAngle) {
                    swing_direction_ = -1;
                } else if (sg90_angle_ <= kSwingMinAngle) {
                    swing_direction_ = 1;
                }
                sg90_angle_ = ClampAngle(sg90_angle_ + swing_direction_ * kSwingStep, kSwingMinAngle, kSwingMaxAngle);
                sg90_.SetAngle(sg90_angle_);
                delay_ms = GetSwingDelayMs(sg90_angle_);
            }
            Unlock();

            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    std::string GetRemoteUrl() {
#ifdef ZHIMU_LUCKY_CAT_SERVER_IP
        return lucky_cat_remote_config::ResolveUrl("");
#else
        Settings settings("lucky_cat", false);
        return lucky_cat_remote_config::ResolveUrl(
            settings.GetString("remote_ws_url", CONFIG_LUCKY_CAT_REMOTE_WS_URL));
#endif
    }

    void RemoteTask() {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int retry_ms = kRemoteInitialRetryMs;

        while (remote_should_run_) {
            const std::string url = GetRemoteUrl();
            if (url.empty()) {
                ESP_LOGI(TAG, "remote WebSocket URL is empty, stopping remote task");
                break;
            }

            auto network = Board::GetInstance().GetNetwork();
            if (network == nullptr) {
                ESP_LOGW(TAG, "network interface is not ready");
                vTaskDelay(pdMS_TO_TICKS(retry_ms));
                retry_ms = std::min(retry_ms * 2, kRemoteMaxRetryMs);
                continue;
            }

            auto websocket = network->CreateWebSocket(3);
            if (websocket == nullptr) {
                ESP_LOGW(TAG, "failed to create remote WebSocket");
                vTaskDelay(pdMS_TO_TICKS(retry_ms));
                retry_ms = std::min(retry_ms * 2, kRemoteMaxRetryMs);
                continue;
            }

            websocket->SetReceiveBufferSize(2048);
            websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
            websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
            websocket->OnData([this, ws = websocket.get()](const char* data, size_t len, bool binary) {
                HandleRemoteData(ws, data, len, binary);
            });
            websocket->OnDisconnected([]() {
                ESP_LOGW(TAG, "remote WebSocket disconnected");
            });

            ESP_LOGI(TAG, "connecting remote WebSocket: %s", url.c_str());
            if (!websocket->Connect(url.c_str())) {
                ESP_LOGW(TAG, "remote WebSocket connect failed, code=%d", websocket->GetLastError());
                vTaskDelay(pdMS_TO_TICKS(retry_ms));
                retry_ms = std::min(retry_ms * 2, kRemoteMaxRetryMs);
                continue;
            }

            retry_ms = kRemoteInitialRetryMs;
            SendHello(websocket.get());
            int ping_ticks = 0;
            while (remote_should_run_ && websocket->IsConnected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (++ping_ticks >= 20) {
                    websocket->Ping();
                    ping_ticks = 0;
                }
            }
            websocket->Close();
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = std::min(retry_ms * 2, kRemoteMaxRetryMs);
        }

        remote_should_run_ = false;
        remote_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

    void SendHello(WebSocket* websocket) {
        std::string hello = "{\"type\":\"hello\",\"role\":\"device\",\"device_id\":\"";
        hello += JsonEscape(SystemInfo::GetMacAddress());
        hello += "\",\"capabilities\":[\"servo\"]}";
        websocket->Send(hello);
    }

    void HandleRemoteData(WebSocket* websocket, const char* data, size_t len, bool binary) {
        if (binary || data == nullptr || len == 0) {
            return;
        }

        std::string message(data, len);
        cJSON* root = cJSON_Parse(message.c_str());
        if (root == nullptr) {
            SendRemoteAck(websocket, 0, false, "invalid json");
            return;
        }

        const cJSON* type = cJSON_GetObjectItem(root, "type");
        const cJSON* seq_item = cJSON_GetObjectItem(root, "seq");
        const int seq = cJSON_IsNumber(seq_item) ? seq_item->valueint : 0;

        if (!cJSON_IsString(type) || strcmp(type->valuestring, "servo_cmd") != 0) {
            cJSON_Delete(root);
            SendRemoteAck(websocket, seq, false, "invalid type");
            return;
        }

        const cJSON* cmd_item = cJSON_GetObjectItem(root, "cmd");
        const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : nullptr;
        if (cmd == nullptr) {
            cJSON_Delete(root);
            SendRemoteAck(websocket, seq, false, "missing cmd");
            return;
        }

        int step = kDefaultStep;
        const cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            const cJSON* step_item = cJSON_GetObjectItem(payload, "step");
            if (cJSON_IsNumber(step_item)) {
                step = step_item->valueint;
            }
        }

        std::string error;
        const bool ok = ExecuteCommand(cmd, step, &error);
        cJSON_Delete(root);
        SendRemoteAck(websocket, seq, ok, error);
    }

    void SendRemoteAck(WebSocket* websocket, int seq, bool ok, const std::string& error) {
        std::string ack = "{\"type\":\"servo_ack\",\"seq\":";
        ack += std::to_string(seq);
        ack += ",\"ok\":";
        ack += ok ? "true" : "false";
        if (!ok) {
            ack += ",\"error\":\"";
            ack += JsonEscape(error);
            ack += "\"";
        }
        ack += ",\"payload\":";
        ack += StatusPayloadJson();
        ack += "}";
        websocket->Send(ack);
    }

    bool ExecuteCommand(const char* cmd, int step, std::string* error) {
        if (cmd == nullptr) {
            *error = "missing cmd";
            return false;
        }
        step = std::max(1, std::min(step, kMaxStep));

        Lock();
        if (!initialized_) {
            Unlock();
            *error = "servo not initialized";
            return false;
        }

        bool ok = true;
        if (strcmp(cmd, "left") == 0) {
            mg996r_angle_ = ClampAngle(mg996r_angle_ - step, kMg996rMinAngle, kMg996rMaxAngle);
            mg996r_.SetAngle(mg996r_angle_);
        } else if (strcmp(cmd, "right") == 0) {
            mg996r_angle_ = ClampAngle(mg996r_angle_ + step, kMg996rMinAngle, kMg996rMaxAngle);
            mg996r_.SetAngle(mg996r_angle_);
        } else if (strcmp(cmd, "up") == 0) {
            swing_enabled_ = false;
            sg90_angle_ = ClampAngle(sg90_angle_ - step, kSg90MinAngle, kSg90MaxAngle);
            sg90_.SetAngle(sg90_angle_);
        } else if (strcmp(cmd, "down") == 0) {
            swing_enabled_ = false;
            sg90_angle_ = ClampAngle(sg90_angle_ + step, kSg90MinAngle, kSg90MaxAngle);
            sg90_.SetAngle(sg90_angle_);
        } else if (strcmp(cmd, "center") == 0) {
            swing_enabled_ = false;
            mg996r_angle_ = kDefaultAngle;
            sg90_angle_ = kDefaultAngle;
            mg996r_.SetAngle(mg996r_angle_);
            sg90_.SetAngle(sg90_angle_);
        } else if (strcmp(cmd, "swing_on") == 0) {
            swing_enabled_ = true;
            sg90_angle_ = ClampAngle(sg90_angle_, kSwingMinAngle, kSwingMaxAngle);
            sg90_.SetAngle(sg90_angle_);
        } else if (strcmp(cmd, "swing_off") == 0) {
            swing_enabled_ = false;
        } else if (strcmp(cmd, "swing_toggle") == 0) {
            swing_enabled_ = !swing_enabled_;
            if (swing_enabled_) {
                sg90_angle_ = ClampAngle(sg90_angle_, kSwingMinAngle, kSwingMaxAngle);
                sg90_.SetAngle(sg90_angle_);
            }
        } else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "stop") == 0) {
            ok = true;
        } else {
            ok = false;
            *error = "invalid cmd";
        }
        Unlock();
        return ok;
    }

    std::string SetAngles(int mg996r_angle, int sg90_angle) {
        Lock();
        swing_enabled_ = false;
        mg996r_angle_ = ClampAngle(mg996r_angle, kMg996rMinAngle, kMg996rMaxAngle);
        sg90_angle_ = ClampAngle(sg90_angle, kSg90MinAngle, kSg90MaxAngle);
        if (initialized_) {
            mg996r_.SetAngle(mg996r_angle_);
            sg90_.SetAngle(sg90_angle_);
        }
        Unlock();
        return StatusJson();
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.servo.move",
            "Move lucky-cat servos by command: left, right, up, down, center, status, swing_on, swing_off, swing_toggle.",
            PropertyList({
                Property("cmd", kPropertyTypeString),
                Property("step", kPropertyTypeInteger, kDefaultStep, 1, kMaxStep),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string error;
                const auto cmd = properties["cmd"].value<std::string>();
                const int step = properties["step"].value<int>();
                if (!ExecuteCommand(cmd.c_str(), step, &error)) {
                    return std::string("{\"ok\":false,\"error\":\"") + JsonEscape(error) + "\"}";
                }
                return StatusJson();
            });

        mcp_server.AddTool(
            "self.servo.set_angle",
            "Set MG996R body angle and SG90 right-arm angle. Both are 0-180 degrees and clamped to safe ranges.",
            PropertyList({
                Property("mg996r", kPropertyTypeInteger, 0, 180),
                Property("sg90", kPropertyTypeInteger, 0, 180),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return SetAngles(properties["mg996r"].value<int>(), properties["sg90"].value<int>());
            });

        mcp_server.AddTool(
            "self.servo.get_status",
            "Get lucky-cat servo status.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return StatusJson();
            });

        ESP_LOGI(TAG, "lucky-cat servo MCP tools registered");
    }

    std::string StatusJson() {
        return std::string("{\"ok\":true,\"payload\":") + StatusPayloadJson() + "}";
    }

    std::string StatusPayloadJson() {
        Lock();
        const bool initialized = initialized_;
        const bool swing = swing_enabled_;
        const int mg996r = mg996r_angle_;
        const int sg90 = sg90_angle_;
        Unlock();

        std::string json = "{\"initialized\":";
        json += initialized ? "true" : "false";
        json += ",\"mg996r\":";
        json += std::to_string(mg996r);
        json += ",\"sg90\":";
        json += std::to_string(sg90);
        json += ",\"swing\":";
        json += swing ? "true" : "false";
        json += ",\"mg996r_gpio\":";
        json += std::to_string(Mg996rGpio());
        json += ",\"sg90_gpio\":";
        json += std::to_string(Sg90Gpio());
        json += "}";
        return json;
    }

    void Lock() {
        if (mutex_ != nullptr) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
        }
    }

    void Unlock() {
        if (mutex_ != nullptr) {
            xSemaphoreGive(mutex_);
        }
    }

    static int ClampAngle(int angle, int min_angle, int max_angle) {
        return std::max(min_angle, std::min(angle, max_angle));
    }

    static int GetSwingDelayMs(int angle) {
        const int distance_to_edge = std::min(angle - kSwingMinAngle, kSwingMaxAngle - angle);
        if (distance_to_edge >= kSwingEndpointEaseDegrees) {
            return kSwingIntervalMs;
        }
        const int eased_extra = (kSwingEndpointEaseDegrees - std::max(0, distance_to_edge)) *
            (kSwingEndpointIntervalMs - kSwingIntervalMs) / kSwingEndpointEaseDegrees;
        return kSwingIntervalMs + eased_extra;
    }
};

LuckyCatServoController* g_controller = nullptr;

}  // namespace

extern "C" void InitializeLuckyCatServoController() {
    if (g_controller == nullptr) {
        g_controller = new LuckyCatServoController();
    }
}

extern "C" bool LuckyCatServoToggleSwing() {
    if (g_controller == nullptr) {
        return false;
    }
    return g_controller->ToggleSwing();
}

extern "C" bool LuckyCatServoIsSwingEnabled() {
    if (g_controller == nullptr) {
        return false;
    }
    return g_controller->IsSwingEnabled();
}
