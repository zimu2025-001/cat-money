#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#if __has_include("camera_private_config.h")
#include "camera_private_config.h"
#endif

#ifndef ZHIMU_CAM_WIFI_SSID
#define ZHIMU_CAM_WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef ZHIMU_CAM_WIFI_PASS
#define ZHIMU_CAM_WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

#ifndef ZHIMU_CAM_UDP_HOST
#define ZHIMU_CAM_UDP_HOST "YOUR_SERVER_IP"
#endif

#ifndef ZHIMU_CAM_UDP_PORT
#define ZHIMU_CAM_UDP_PORT 5000
#endif

#define WIFI_SSID ZHIMU_CAM_WIFI_SSID
#define WIFI_PASS ZHIMU_CAM_WIFI_PASS
#define UDP_HOST ZHIMU_CAM_UDP_HOST
#define UDP_PORT ZHIMU_CAM_UDP_PORT
#define UDP_MAX_PAYLOAD_BYTES 1400
#define UDP_CHUNK_DELAY_MS 20
#define FRAME_DELAY_MS 1
#define WIFI_RECONNECT_DELAY_MS 2000
#define CAMERA_RETRY_DELAY_MS 5000
#define WIFI_MAX_TX_POWER 78
#define STREAM_LOG_EVERY_N_FRAMES 60

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "cam_udp_raw";
static EventGroupHandle_t s_wifi_event_group;
static uint32_t s_frame_id;
static uint32_t s_sent_count;
static uint32_t s_drop_count;

static camera_config_t camera_config = {
    .pin_pwdn = -1,
    .pin_reset = -1,
    .pin_xclk = 15,
    .pin_sccb_sda = 4,
    .pin_sccb_scl = 5,
    .pin_d7 = 16,
    .pin_d6 = 17,
    .pin_d5 = 18,
    .pin_d4 = 12,
    .pin_d3 = 10,
    .pin_d2 = 8,
    .pin_d1 = 9,
    .pin_d0 = 11,
    .pin_vsync = 6,
    .pin_href = 7,
    .pin_pclk = 13,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 12,
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static void delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ms > 0 && ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static bool is_complete_jpeg(const camera_fb_t *frame)
{
    return frame != NULL &&
           frame->len >= 4 &&
           frame->buf[0] == 0xff &&
           frame->buf[1] == 0xd8 &&
           frame->buf[frame->len - 2] == 0xff &&
           frame->buf[frame->len - 1] == 0xd9;
}

static void log_camera_config(void)
{
    ESP_LOGI(TAG,
             "camera pins: pwdn=%d reset=%d xclk=%d sda=%d scl=%d d0=%d d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d vsync=%d href=%d pclk=%d",
             camera_config.pin_pwdn,
             camera_config.pin_reset,
             camera_config.pin_xclk,
             camera_config.pin_sccb_sda,
             camera_config.pin_sccb_scl,
             camera_config.pin_d0,
             camera_config.pin_d1,
             camera_config.pin_d2,
             camera_config.pin_d3,
             camera_config.pin_d4,
             camera_config.pin_d5,
             camera_config.pin_d6,
             camera_config.pin_d7,
             camera_config.pin_vsync,
             camera_config.pin_href,
             camera_config.pin_pclk);
    ESP_LOGI(TAG,
             "camera stream: raw UDP JPEG bytes frame=VGA quality=%d payload=%d chunk_delay=%d ms",
             camera_config.jpeg_quality,
             UDP_MAX_PAYLOAD_BYTES,
             UDP_CHUNK_DELAY_MS);
}

static void apply_wifi_performance_mode(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to disable Wi-Fi power save: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to request Wi-Fi HT40 bandwidth: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to set Wi-Fi tx power: %s", esp_err_to_name(err));
    }
}

static void apply_camera_tuning(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGW(TAG, "camera sensor handle is unavailable");
        return;
    }

    if (sensor->set_framesize) {
        sensor->set_framesize(sensor, camera_config.frame_size);
    }
    if (sensor->set_quality) {
        sensor->set_quality(sensor, camera_config.jpeg_quality);
    }
    if (sensor->set_brightness) {
        sensor->set_brightness(sensor, 0);
    }
    if (sensor->set_contrast) {
        sensor->set_contrast(sensor, 1);
    }
    if (sensor->set_saturation) {
        sensor->set_saturation(sensor, 0);
    }
    if (sensor->set_whitebal) {
        sensor->set_whitebal(sensor, 1);
    }
    if (sensor->set_awb_gain) {
        sensor->set_awb_gain(sensor, 1);
    }
    if (sensor->set_exposure_ctrl) {
        sensor->set_exposure_ctrl(sensor, 1);
    }
    if (sensor->set_gain_ctrl) {
        sensor->set_gain_ctrl(sensor, 1);
    }
    if (sensor->set_lenc) {
        sensor->set_lenc(sensor, 1);
    }
    if (sensor->set_bpc) {
        sensor->set_bpc(sensor, 1);
    }
    if (sensor->set_wpc) {
        sensor->set_wpc(sensor, 1);
    }
    if (sensor->set_raw_gma) {
        sensor->set_raw_gma(sensor, 1);
    }
    if (sensor->set_dcw) {
        sensor->set_dcw(sensor, 1);
    }

    ESP_LOGI(TAG, "OV2640 tuned for raw UDP monitoring stream");
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_wifi_sta(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_wifi_performance_mode();

    ESP_LOGI(TAG, "connecting to Wi-Fi SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

static esp_err_t init_camera(void)
{
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "camera initialized");
    apply_camera_tuning();
    return ESP_OK;
}

static void warmup_camera_frames(void)
{
    for (int i = 0; i < 3; i++) {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame != NULL) {
            esp_camera_fb_return(frame);
        }
        delay_ms(30);
    }
}

static int create_udp_socket(struct sockaddr_in *dest_addr)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "unable to create UDP socket: errno %d", errno);
        return -1;
    }

    int sndbuf = 32 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    memset(dest_addr, 0, sizeof(*dest_addr));
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(UDP_PORT);
    if (inet_pton(AF_INET, UDP_HOST, &dest_addr->sin_addr) != 1) {
        ESP_LOGE(TAG, "invalid UDP host: %s", UDP_HOST);
        close(sock);
        return -1;
    }

    return sock;
}

static bool send_udp_packet(int sock, const struct sockaddr_in *dest_addr, const uint8_t *data, size_t len)
{
    int sent = sendto(sock, data, len, 0, (const struct sockaddr *)dest_addr, sizeof(*dest_addr));
    return sent == (int)len;
}

static void send_frame_via_raw_udp(int sock, const struct sockaddr_in *dest_addr)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Wi-Fi is offline, waiting before next frame");
        delay_ms(WIFI_RECONNECT_DELAY_MS);
        return;
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        ESP_LOGE(TAG, "failed to capture frame");
        delay_ms(20);
        return;
    }

    if (!is_complete_jpeg(frame)) {
        ESP_LOGW(TAG, "dropping incomplete JPEG frame: bytes=%u", (unsigned)frame->len);
        esp_camera_fb_return(frame);
        delay_ms(20);
        return;
    }

    size_t sent_bytes = 0;
    bool dropped = false;
    uint32_t frame_id = ++s_frame_id;

    while (sent_bytes < frame->len) {
        size_t len = frame->len - sent_bytes;
        if (len > UDP_MAX_PAYLOAD_BYTES) {
            len = UDP_MAX_PAYLOAD_BYTES;
        }

        if (!send_udp_packet(sock, dest_addr, frame->buf + sent_bytes, len)) {
            dropped = true;
            break;
        }

        sent_bytes += len;
        delay_ms(UDP_CHUNK_DELAY_MS);
    }

    if (dropped) {
        s_drop_count++;
        ESP_LOGW(TAG, "UDP raw frame dropped: id=%lu errno=%d bytes=%u sent=%u dropped=%lu",
                 (unsigned long)frame_id,
                 errno,
                 (unsigned)frame->len,
                 (unsigned)sent_bytes,
                 (unsigned long)s_drop_count);
        delay_ms(100);
    } else {
        s_sent_count++;
        if (frame_id <= 3 || (s_sent_count % STREAM_LOG_EVERY_N_FRAMES) == 0) {
            ESP_LOGI(TAG,
                     "UDP raw frame sent: id=%lu bytes=%u sent=%lu dropped=%lu",
                     (unsigned long)frame_id,
                     (unsigned)frame->len,
                     (unsigned long)s_sent_count,
                     (unsigned long)s_drop_count);
        }
    }

    esp_camera_fb_return(frame);
    delay_ms(FRAME_DELAY_MS);
}

void app_main(void)
{
    struct sockaddr_in dest_addr;
    int sock;
    esp_err_t err;

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_wifi_sta());
    log_camera_config();

    while (true) {
        err = init_camera();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG,
                 "camera probe failed, will retry in %d ms. This is not a Wi-Fi failure; check camera model, ribbon cable, pin map, and PWDN/RESET wiring.",
                 CAMERA_RETRY_DELAY_MS);
        delay_ms(CAMERA_RETRY_DELAY_MS);
    }

    sock = create_udp_socket(&dest_addr);
    if (sock < 0) {
        return;
    }

    ESP_LOGI(TAG, "starting raw UDP camera stream to %s:%d", UDP_HOST, UDP_PORT);
    warmup_camera_frames();

    while (true) {
        send_frame_via_raw_udp(sock, &dest_addr);
    }
}
