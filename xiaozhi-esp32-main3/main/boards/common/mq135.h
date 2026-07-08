#ifndef MQ135_H
#define MQ135_H

#include <driver/gpio.h>
#include <driver/adc.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <functional>

/**
 * MQ135 gas/smoke sensor ADC driver
 *
 * The MQ135 sensor detects various gases including:
 * - CO2 (Carbon Dioxide)
 * - NH3 (Ammonia)
 * - NOx (Nitrogen Oxides)
 * - Alcohol, Benzene, Smoke
 *
 * The board-specific code can use either calibrated PPM helpers or raw ADC
 * values. Zhimu uses the raw ADC value as a smoke alarm signal.
 *
 * Preheat time: At least 24-48 hours for optimal accuracy
 * For initial use, preheat for at least 5-10 minutes
 */
class Mq135Sensor {
public:
    /**
     * Constructor for MQ135 sensor
     * @param adc_unit ADC unit (ADC_UNIT_1 or ADC_UNIT_2)
     * @param adc_channel ADC channel
     * @param rload Load resistance in ohms (default: 10k ohms)
     * @param rzero Calibration resistance in clean air (default: 76.63k ohms for CO2)
     */
    Mq135Sensor(adc_unit_t adc_unit, adc_channel_t adc_channel,
                float rload = 10000.0f, float rzero = 76360.0f);
    ~Mq135Sensor();

    /**
     * Get current PPM (parts per million) value
     * This is calibrated for CO2 by default
     * @return PPM value
     */
    float GetPpm();

    /**
     * Get corrected PPM for temperature and humidity
     * @param temperature Temperature in Celsius
     * @param humidity Relative humidity percentage
     * @return Corrected PPM value
     */
    float GetCorrectedPpm(float temperature, float humidity);

    /**
     * Get raw ADC value
     * @return Raw ADC reading (0-4095 for 12-bit ADC)
     */
    int GetRawValue();

    /**
     * Get resistance of the sensor
     * @return Sensor resistance in ohms
     */
    float GetResistance();

    /**
     * Calibrate the sensor in clean air
     * Call this method after preheating in fresh air
     * This sets R0 to the current resistance value
     * @return The new R0 value
     */
    float CalibrateInCleanAir();

    /**
     * Set the R0 value manually
     * @param rzero New R0 value in ohms
     */
    void SetRZero(float rzero);

    /**
     * Get current R0 value
     * @return Current R0 value in ohms
     */
    float GetRZero() const { return rzero_; }

    /**
     * Check if sensor is ready (warmed up)
     * @param elapsed_ms Time since power-on in milliseconds
     * @return true if sensor should be ready
     */
    static bool IsWarmedUp(int64_t elapsed_ms) {
        return elapsed_ms > 300000; // 5 minutes minimum warm-up
    }

    /**
     * Get air quality level based on CO2 PPM
     * @param ppm CO2 concentration in PPM
     * @return Quality level string
     */
    static const char* GetAirQualityLevel(float ppm);

    /**
     * Register callback for periodic readings
     * @param interval_ms Reading interval in milliseconds
     * @param callback Callback function receiving PPM value
     */
    void StartPeriodicReading(uint32_t interval_ms, std::function<void(float ppm)> callback);

    /**
     * Stop periodic readings
     */
    void StopPeriodicReading();

private:
    adc_oneshot_unit_handle_t adc_handle_;
    adc_channel_t adc_channel_;
    adc_unit_t adc_unit_;
    float rload_;           // Load resistance
    float rzero_;           // Calibration resistance in clean air
    esp_timer_handle_t timer_handle_ = nullptr;
    std::function<void(float)> on_reading_callback_;

    // Parameters for gas detection curve (for CO2)
    // The curve is: ppm = A * (Rs/R0)^B
    // For CO2: A = 116.6020682, B = -2.769034857
    static constexpr float PARAM_A = 116.6020682f;
    static constexpr float PARAM_B = -2.769034857f;

    /**
     * Calculate PPM from resistance ratio
     * @param resistance Sensor resistance
     * @return PPM value
     */
    float CalculatePpm(float resistance);

    /**
     * Get correction factor for temperature and humidity
     * @param temperature Temperature in Celsius
     * @param humidity Relative humidity percentage
     * @return Correction factor
     */
    float GetCorrectionFactor(float temperature, float humidity);
};

#endif // MQ135_H
