#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define SCREEN_BUTTON_GPIO      GPIO_NUM_2
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

// Zhimu 2.25 inch SPI TFT ST7789 display pins
// GPIO 8/9 are reserved for the ultrasonic sensor.
#define DISPLAY_SPI_SCK_PIN   GPIO_NUM_42  // SPI时钟 (屏幕SCL)
#define DISPLAY_SPI_MOSI_PIN  GPIO_NUM_41  // SPI数据 (屏幕SDA)
#define DISPLAY_SPI_CLOCK_HZ  (10 * 1000 * 1000)
#define DISPLAY_DC_PIN        GPIO_NUM_17  // 数据/命令选择
#define DISPLAY_CS_PIN        GPIO_NUM_45  // 片选 (改用GPIO45)
#define DISPLAY_RES_PIN       GPIO_NUM_46  // 复位 (改用GPIO46)
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_47  // 背光控制 (改用GPIO47)

// Servo signal pins for the lucky-cat body.
#define MG996R_SERVO_GPIO GPIO_NUM_20
#define SG90_SERVO_GPIO   GPIO_NUM_11

// HC-SR04 超声波传感器引脚
#define HC_SR04_TRIG_PIN        GPIO_NUM_8
#define HC_SR04_ECHO_PIN        GPIO_NUM_9

// 门口检测配置
#define DOOR_MONITOR_DETECTION_THRESHOLD_CM  50
#define DOOR_MONITOR_COOLDOWN_MS             5000
#define DOOR_MONITOR_MOTION_DELTA_CM         5
#define DOOR_MONITOR_NOISE_IGNORE_CM         2
#define DOOR_MONITOR_MOTION_CONFIRM_COUNT    2
#define DOOR_MONITOR_PRESENCE_LOST_MS        1500
#define DOOR_MONITOR_AUTO_IDLE_MS            10000
#define DOOR_MONITOR_AUTO_IDLE_CHECK_MS      500
#define DOOR_MONITOR_REARM_IDLE_MS           10000
#define DOOR_MONITOR_REARM_CHECK_MS          500

// DHT11 温湿度传感器引脚
#define DHT11_PIN        GPIO_NUM_13  // 数据引脚 (改用GPIO13)
#define DHT11_TYPE_DHT22 0            // 0 = DHT11, 1 = DHT22

// MQ135 烟雾报警传感器引脚，硬件接口保持不变
#define MQ135_ADC_UNIT   ADC_UNIT_1
#define MQ135_ADC_CHAN   ADC_CHANNEL_0  // GPIO1

// 烟雾报警配置：常见 MQ 模块烟雾越浓 ADC 越高。如模块方向相反，将该值改为 0。
#define SMOKE_ALARM_HIGHER_RAW_IS_DANGER 1
#define SMOKE_ALARM_ADC_THRESHOLD         1800
#define SMOKE_ALARM_ADC_HYSTERESIS        180
#define SMOKE_ALARM_READ_INTERVAL_MS      3000
#define SMOKE_ALARM_REPEAT_MS             15000
#define SMOKE_ALARM_TEST_ON_BOOT          0
#define SMOKE_ALARM_TEST_RAW              4095
#define SMOKE_ALARM_TEST_DELAY_MS         5000

// 2.25 inch strip TFT ST7789 - landscape mode.
// Current logical resolution: 284 x 76.
#define DISPLAY_WIDTH   284
#define DISPLAY_HEIGHT  76
#define DISPLAY_SWAP_XY true
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
// 横屏偏移参数 (从STM32示例: TFT_COLUMN_OFFSET=18, TFT_LINE_OFFSET=82)
#define DISPLAY_OFFSET_X  18
#define DISPLAY_OFFSET_Y  82
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#endif // _BOARD_CONFIG_H_
