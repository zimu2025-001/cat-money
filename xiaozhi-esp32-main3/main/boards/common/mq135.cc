#include "mq135.h"
#include "esp_log.h"
#include <cmath>

static const char *TAG = "MQ135";

Mq135Sensor::Mq135Sensor(adc_unit_t adc_unit, adc_channel_t adc_channel,
                         float rload, float rzero)
    : adc_channel_(adc_channel)
    , adc_unit_(adc_unit)
    , rload_(rload)
    , rzero_(rzero) {

    // Initialize ADC handle
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = adc_unit,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

    // Configure ADC channel
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,    // 0-3.3V range
        .bitwidth = ADC_BITWIDTH_12, // 12-bit resolution
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &config));

    ESP_LOGI(TAG, "MQ135 initialized on ADC unit %d, channel %d", adc_unit, adc_channel);
}

Mq135Sensor::~Mq135Sensor() {
    StopPeriodicReading();
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
    }
}

int Mq135Sensor::GetRawValue() {
    int raw_value = 0;
    esp_err_t err = adc_oneshot_read(adc_handle_, adc_channel_, &raw_value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ADC: %s", esp_err_to_name(err));
        return 0;
    }
    return raw_value;
}

float Mq135Sensor::GetResistance() {
    int raw = GetRawValue();
    if (raw == 0) {
        ESP_LOGW(TAG, "ADC raw value is 0, check sensor connection");
        return 0;
    }

    // The sensor forms a voltage divider with the load resistor
    // Vout = Vin * Rload / (Rs + Rload)
    // Rs = Rload * (Vin - Vout) / Vout
    // With 12-bit ADC: Vout = raw * Vref / 4095
    // Assuming Vref = 3.3V and Vin = 3.3V (connected to same supply)
    // Rs = Rload * (4095 - raw) / raw

    float resistance = rload_ * (4095.0f - (float)raw) / (float)raw;
    return resistance;
}

float Mq135Sensor::CalculatePpm(float resistance) {
    if (resistance <= 0 || rzero_ <= 0) {
        return 0;
    }

    // Using the power law: ppm = A * (Rs/R0)^B
    // For CO2 detection
    float ratio = resistance / rzero_;
    float ppm = PARAM_A * powf(ratio, PARAM_B);

    // Clamp to reasonable range
    if (ppm < 0) ppm = 0;
    if (ppm > 10000) ppm = 10000; // Max measurable

    return ppm;
}

float Mq135Sensor::GetPpm() {
    float resistance = GetResistance();
    return CalculatePpm(resistance);
}

float Mq135Sensor::GetCorrectionFactor(float temperature, float humidity) {
    // Correction factor based on temperature and humidity
    // The MQ135 is sensitive to temperature and humidity changes
    // Reference conditions: 20°C, 65% RH

    // Temperature correction (linear approximation)
    // Factor increases with temperature
    float temp_factor = 1.0f + (temperature - 20.0f) * 0.01f;

    // Humidity correction
    // Factor increases with humidity
    float humidity_factor = 1.0f + (humidity - 65.0f) * 0.005f;

    return temp_factor * humidity_factor;
}

float Mq135Sensor::GetCorrectedPpm(float temperature, float humidity) {
    float ppm = GetPpm();
    float correction = GetCorrectionFactor(temperature, humidity);
    return ppm / correction;
}

float Mq135Sensor::CalibrateInCleanAir() {
    // In clean air, CO2 concentration is approximately 400 PPM
    // We can calculate R0 from this known concentration
    // ppm = A * (Rs/R0)^B
    // R0 = Rs / (ppm/A)^(1/B)

    float resistance = GetResistance();
    if (resistance <= 0) {
        ESP_LOGW(TAG, "Invalid resistance during calibration");
        return rzero_;
    }

    // Calculate R0 for 400 PPM in clean air
    float ratio = powf(400.0f / PARAM_A, 1.0f / PARAM_B);
    rzero_ = resistance / ratio;

    ESP_LOGI(TAG, "Calibration complete. New R0 = %.2f ohms", rzero_);
    return rzero_;
}

void Mq135Sensor::SetRZero(float rzero) {
    rzero_ = rzero;
    ESP_LOGI(TAG, "R0 set to %.2f ohms", rzero_);
}

const char* Mq135Sensor::GetAirQualityLevel(float ppm) {
    if (ppm < 400) {
        return "优";
    } else if (ppm < 600) {
        return "良";
    } else if (ppm < 800) {
        return "中";
    } else if (ppm < 1000) {
        return "一般";
    } else if (ppm < 1500) {
        return "差";
    } else if (ppm < 2000) {
        return "很差";
    } else {
        return "危险";
    }
}

void Mq135Sensor::StartPeriodicReading(uint32_t interval_ms, std::function<void(float ppm)> callback) {
    // Stop any existing timer
    StopPeriodicReading();

    on_reading_callback_ = callback;

    esp_timer_create_args_t timer_cfg = {
        .callback = [](void *arg) {
            Mq135Sensor *self = (Mq135Sensor *)arg;
            float ppm = self->GetPpm();
            if (self->on_reading_callback_) {
                self->on_reading_callback_(ppm);
            }
        },
        .arg = this,
        .name = "mq135_timer",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &timer_handle_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, interval_ms * 1000));

    ESP_LOGI(TAG, "Started periodic readings every %lu ms", interval_ms);
}

void Mq135Sensor::StopPeriodicReading() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
        ESP_LOGI(TAG, "Stopped periodic readings");
    }
}
