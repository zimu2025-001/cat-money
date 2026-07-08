#include "sg90_servo.h"

#include <cstring>
#include <string>

#include <cJSON.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "mcp_server.h"

#define TAG "PanTiltServo"

#ifdef CONFIG_SG90_SERVO_GPIO
#define PAN_SERVO_GPIO ((gpio_num_t)CONFIG_SG90_SERVO_GPIO)
#else
#define PAN_SERVO_GPIO GPIO_NUM_18
#endif

#ifdef CONFIG_SG90_SERVO_TILT_GPIO
#define TILT_SERVO_GPIO ((gpio_num_t)CONFIG_SG90_SERVO_TILT_GPIO)
#else
#define TILT_SERVO_GPIO GPIO_NUM_19
#endif

#ifdef CONFIG_SG90_SERVO_UART_TX_GPIO
#define SERVO_UART_TX_GPIO ((gpio_num_t)CONFIG_SG90_SERVO_UART_TX_GPIO)
#else
#define SERVO_UART_TX_GPIO GPIO_NUM_17
#endif

#ifdef CONFIG_SG90_SERVO_UART_RX_GPIO
#define SERVO_UART_RX_GPIO ((gpio_num_t)CONFIG_SG90_SERVO_UART_RX_GPIO)
#else
#define SERVO_UART_RX_GPIO GPIO_NUM_16
#endif

#ifdef CONFIG_SG90_SERVO_UART_BAUD_RATE
#define SERVO_UART_BAUD_RATE CONFIG_SG90_SERVO_UART_BAUD_RATE
#else
#define SERVO_UART_BAUD_RATE 115200
#endif

class SG90ServoController {
public:
    SG90ServoController()
        : initialized_(false),
          pan_angle_(90),
          tilt_angle_(90),
          mutex_(xSemaphoreCreateMutex()) {
        initialized_ = pan_.Init(PAN_SERVO_GPIO, LEDC_TIMER_0, LEDC_CHANNEL_0, pan_angle_) &&
                       tilt_.Init(TILT_SERVO_GPIO, LEDC_TIMER_0, LEDC_CHANNEL_1, tilt_angle_);
        RegisterMcpTools();
        StartUart();
    }

    bool IsInitialized() const {
        return initialized_;
    }

private:
    SG90Servo pan_;
    SG90Servo tilt_;
    bool initialized_;
    int pan_angle_;
    int tilt_angle_;
    SemaphoreHandle_t mutex_;

    static void UartTaskEntry(void* arg) {
        static_cast<SG90ServoController*>(arg)->UartTask();
    }

    void StartUart() {
        uart_config_t uart_config = {};
        uart_config.baud_rate = SERVO_UART_BAUD_RATE;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.source_clk = UART_SCLK_DEFAULT;

        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 2048, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, SERVO_UART_TX_GPIO, SERVO_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

        xTaskCreate(UartTaskEntry, "pan_tilt_uart", 4096, this, 5, nullptr);
        ESP_LOGI(TAG, "UART control enabled on tx=%d rx=%d baud=%d", SERVO_UART_TX_GPIO, SERVO_UART_RX_GPIO, SERVO_UART_BAUD_RATE);
    }

    void UartTask() {
        uint8_t byte = 0;
        std::string line;
        line.reserve(256);

        while (true) {
            const int len = uart_read_bytes(UART_NUM_1, &byte, 1, pdMS_TO_TICKS(200));
            if (len <= 0) {
                continue;
            }
            if (byte == '\r') {
                continue;
            }
            if (byte == '\n') {
                if (!line.empty()) {
                    HandleSerialLine(line);
                    line.clear();
                }
                continue;
            }
            if (line.size() < 512) {
                line.push_back(static_cast<char>(byte));
            } else {
                line.clear();
                SendSerialError(0, "line too long");
            }
        }
    }

    void HandleSerialLine(const std::string& line) {
        cJSON* root = cJSON_Parse(line.c_str());
        if (root == nullptr) {
            SendSerialError(0, "invalid json");
            return;
        }

        const cJSON* seq_item = cJSON_GetObjectItem(root, "seq");
        const int seq = cJSON_IsNumber(seq_item) ? seq_item->valueint : 0;
        const cJSON* cmd_item = cJSON_GetObjectItem(root, "cmd");
        const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : nullptr;
        cJSON* payload = cJSON_GetObjectItem(root, "payload");

        if (cmd == nullptr) {
            cJSON_Delete(root);
            SendSerialError(seq, "missing cmd");
            return;
        }

        int step = 5;
        if (cJSON_IsObject(payload)) {
            const cJSON* step_item = cJSON_GetObjectItem(payload, "step");
            if (cJSON_IsNumber(step_item)) {
                step = step_item->valueint;
            }
        }

        std::string error;
        const bool ok = ExecuteCommand(cmd, step, &error);
        SendSerialAck(seq, ok, error.c_str());
        cJSON_Delete(root);
    }

    bool ExecuteCommand(const char* cmd, int step, std::string* error) {
        if (!initialized_) {
            *error = "servo not initialized";
            return false;
        }
        if (step < 1) {
            step = 1;
        }
        if (step > 45) {
            step = 45;
        }

        Lock();
        bool ok = true;
        if (strcmp(cmd, "left") == 0) {
            pan_angle_ -= step;
        } else if (strcmp(cmd, "right") == 0) {
            pan_angle_ += step;
        } else if (strcmp(cmd, "up") == 0) {
            tilt_angle_ -= step;
        } else if (strcmp(cmd, "down") == 0) {
            tilt_angle_ += step;
        } else if (strcmp(cmd, "center") == 0) {
            pan_angle_ = 90;
            tilt_angle_ = 90;
        } else if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "status") == 0) {
            ok = true;
        } else {
            *error = "invalid cmd";
            ok = false;
        }

        pan_angle_ = ClampAngle(pan_angle_);
        tilt_angle_ = ClampAngle(tilt_angle_);
        if (ok) {
            pan_.SetAngle(pan_angle_);
            tilt_.SetAngle(tilt_angle_);
        }
        Unlock();
        return ok;
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.servo.set_angle",
            "Set pan/tilt servo angles. pan and tilt are 0-180 degrees.",
            PropertyList({
                Property("pan", kPropertyTypeInteger, 90, 0, 180),
                Property("tilt", kPropertyTypeInteger, 90, 0, 180),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!initialized_) {
                    return "{\"ok\":false,\"error\":\"servo not initialized\"}";
                }
                Lock();
                pan_angle_ = ClampAngle(properties["pan"].value<int>());
                tilt_angle_ = ClampAngle(properties["tilt"].value<int>());
                pan_.SetAngle(pan_angle_);
                tilt_.SetAngle(tilt_angle_);
                Unlock();
                return StatusJson();
            });

        mcp_server.AddTool(
            "self.servo.move",
            "Move pan/tilt camera by command: up, down, left, right, stop, center, status.",
            PropertyList({
                Property("cmd", kPropertyTypeString),
                Property("step", kPropertyTypeInteger, 5, 1, 45),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string error;
                const auto cmd = properties["cmd"].value<std::string>();
                const int step = properties["step"].value<int>();
                if (!ExecuteCommand(cmd.c_str(), step, &error)) {
                    return std::string("{\"ok\":false,\"error\":\"") + error + "\"}";
                }
                return StatusJson();
            });

        mcp_server.AddTool(
            "self.servo.get_status",
            "Get pan/tilt servo status.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return StatusJson();
            });

        ESP_LOGI(TAG, "pan/tilt servo MCP tools registered");
    }

    std::string StatusJson() {
        Lock();
        const int pan = pan_angle_;
        const int tilt = tilt_angle_;
        const bool initialized = initialized_;
        Unlock();

        return std::string("{\"ok\":true,\"initialized\":") +
            (initialized ? "true" : "false") +
            ",\"pan\":" + std::to_string(pan) +
            ",\"tilt\":" + std::to_string(tilt) +
            ",\"pan_gpio\":" + std::to_string(PAN_SERVO_GPIO) +
            ",\"tilt_gpio\":" + std::to_string(TILT_SERVO_GPIO) +
            "}";
    }

    void SendSerialAck(int seq, bool ok, const char* error) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "ack");
        cJSON_AddNumberToObject(root, "seq", seq);
        cJSON_AddBoolToObject(root, "ok", ok);
        if (!ok && error != nullptr) {
            cJSON_AddStringToObject(root, "error", error);
        }
        AddStatusPayload(root);
        WriteJsonLine(root);
        cJSON_Delete(root);
    }

    void SendSerialError(int seq, const char* error) {
        SendSerialAck(seq, false, error);
    }

    void AddStatusPayload(cJSON* root) {
        Lock();
        const int pan = pan_angle_;
        const int tilt = tilt_angle_;
        const bool initialized = initialized_;
        Unlock();

        cJSON* payload = cJSON_CreateObject();
        cJSON_AddNumberToObject(payload, "pan", pan);
        cJSON_AddNumberToObject(payload, "tilt", tilt);
        cJSON_AddBoolToObject(payload, "initialized", initialized);
        cJSON_AddItemToObject(root, "payload", payload);
    }

    void WriteJsonLine(cJSON* root) {
        char* text = cJSON_PrintUnformatted(root);
        if (text == nullptr) {
            return;
        }
        uart_write_bytes(UART_NUM_1, text, strlen(text));
        uart_write_bytes(UART_NUM_1, "\n", 1);
        cJSON_free(text);
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

    static int ClampAngle(int angle) {
        if (angle < 0) {
            return 0;
        }
        if (angle > 180) {
            return 180;
        }
        return angle;
    }
};

static SG90ServoController* g_sg90_servo_controller = nullptr;

extern "C" void InitializeSG90ServoController() {
    if (g_sg90_servo_controller == nullptr) {
        g_sg90_servo_controller = new SG90ServoController();
        ESP_LOGI(TAG, "pan/tilt servo controller initialized");
    }
}

extern "C" SG90ServoController* GetSG90ServoController() {
    return g_sg90_servo_controller;
}
