#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "cam_udp_protocol.h"

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

#ifndef ZHIMU_CAM_TARGET_FPS
#define ZHIMU_CAM_TARGET_FPS 15
#endif

#ifndef ZHIMU_CAM_SEND_BUDGET_MS
#define ZHIMU_CAM_SEND_BUDGET_MS 100
#endif

#if ZHIMU_CAM_TARGET_FPS < 1 || ZHIMU_CAM_TARGET_FPS > 60
#error "ZHIMU_CAM_TARGET_FPS must be between 1 and 60"
#endif
#if ZHIMU_CAM_SEND_BUDGET_MS < 1 || ZHIMU_CAM_SEND_BUDGET_MS > 1000
#error "ZHIMU_CAM_SEND_BUDGET_MS must be between 1 and 1000"
#endif

#define WIFI_SSID ZHIMU_CAM_WIFI_SSID
#define WIFI_PASS ZHIMU_CAM_WIFI_PASS
#define UDP_HOST ZHIMU_CAM_UDP_HOST
#define UDP_PORT ZHIMU_CAM_UDP_PORT
#define WIFI_RECONNECT_DELAY_MS 2000
#define CAMERA_RETRY_DELAY_MS 5000
#define WIFI_MAX_TX_POWER 78
#define STREAM_LOG_INTERVAL_US (5LL * 1000000)

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "cam_udp_zmc2";
static EventGroupHandle_t s_wifi_event_group;
static uint32_t s_frame_id;
static uint8_t s_udp_packet[CAM_UDP_DATAGRAM_BYTES];
static struct {
    int64_t start_us;
    uint32_t captured;
    uint32_t sent;
    uint32_t dropped;
    uint32_t capture_errors;
    uint32_t invalid;
    uint32_t retries;
    int last_errno;
    uint64_t sent_bytes;
    uint64_t capture_us;
    uint64_t send_us;
    uint64_t max_send_us;
} s_stats;

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
           frame->buf != NULL &&
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
             "camera stream: ZMC2 frame=VGA quality=%d datagram=%u payload=%u target_fps=%d send_budget=%d ms",
             camera_config.jpeg_quality,
             (unsigned)CAM_UDP_DATAGRAM_BYTES,
             (unsigned)CAM_UDP_PAYLOAD_BYTES,
             ZHIMU_CAM_TARGET_FPS,
             ZHIMU_CAM_SEND_BUDGET_MS);
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

    ESP_LOGI(TAG, "OV2640 tuned for ZMC2 UDP monitoring stream");
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

    /* Never block indefinitely if the Wi-Fi transmit queue is full. */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "unable to make UDP socket nonblocking: errno %d", errno);
        close(sock);
        return -1;
    }

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

static bool send_udp_packet(int sock, const struct sockaddr_in *dest_addr,
                            const uint8_t *data, size_t len, int64_t deadline_us)
{
    while (true) {
        if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
            s_stats.last_errno = ENETDOWN;
            return false;
        }
        if (esp_timer_get_time() >= deadline_us) {
            s_stats.last_errno = ETIMEDOUT;
            return false;
        }

        int sent = sendto(sock, data, len, 0,
                          (const struct sockaddr *)dest_addr, sizeof(*dest_addr));
        if (sent == (int)len) {
            return true;
        }

        int send_errno = sent < 0 ? errno : EIO;
        s_stats.last_errno = send_errno;
        if (send_errno != ENOMEM && send_errno != ENOBUFS &&
            send_errno != EAGAIN && send_errno != EWOULDBLOCK &&
            send_errno != EINTR) {
            return false;
        }
        /* Retry only this unqueued packet and only within this frame's budget.
         * Yield to the network task under pressure, not after every packet. */
        s_stats.retries++;
        delay_ms(1);
    }
}

static void send_frame_via_udp(int sock, const struct sockaddr_in *dest_addr)
{
    if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
        delay_ms(WIFI_RECONNECT_DELAY_MS);
        return;
    }

    int64_t capture_start_us = esp_timer_get_time();
    camera_fb_t *frame = esp_camera_fb_get();
    s_stats.capture_us += esp_timer_get_time() - capture_start_us;
    if (frame == NULL) {
        s_stats.capture_errors++;
        return;
    }

    s_stats.captured++;
    uint32_t frame_id = ++s_frame_id;
    uint16_t chunk_count = cam_udp_chunk_count(frame->len);
    if (!is_complete_jpeg(frame) || chunk_count == 0) {
        s_stats.invalid++;
        s_stats.dropped++;
        esp_camera_fb_return(frame);
        return;
    }

    int64_t send_start_us = esp_timer_get_time();
    int64_t deadline_us = send_start_us + (int64_t)ZHIMU_CAM_SEND_BUDGET_MS * 1000;
    uint32_t frame_crc = cam_udp_crc32(frame->buf, frame->len);
    bool sent_frame = true;
    for (uint16_t chunk = 0; chunk < chunk_count; ++chunk) {
        size_t packet_len = cam_udp_encode_chunk(s_udp_packet, sizeof(s_udp_packet),
                                                 frame->buf, frame->len,
                                                 frame_id, frame_crc, chunk);
        if (packet_len == 0 ||
            !send_udp_packet(sock, dest_addr, s_udp_packet, packet_len, deadline_us)) {
            sent_frame = false;
            break;
        }
    }

    uint64_t send_us = (uint64_t)(esp_timer_get_time() - send_start_us);
    s_stats.send_us += send_us;
    if (send_us > s_stats.max_send_us) {
        s_stats.max_send_us = send_us;
    }
    if (sent_frame) {
        s_stats.sent++;
        s_stats.sent_bytes += frame->len;
    } else {
        /* Do not resend old frames: the receiver discards partial/CRC-bad frames. */
        s_stats.dropped++;
    }
    /* Return the camera buffer before any pacing wait, on every captured path. */
    esp_camera_fb_return(frame);
}

static void log_stream_stats(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_stats.start_us;
    if (elapsed_us < STREAM_LOG_INTERVAL_US) {
        return;
    }
    uint32_t capture_attempts = s_stats.captured + s_stats.capture_errors;
    uint32_t send_attempts = s_stats.captured - s_stats.invalid;
    ESP_LOGI(TAG,
             "stream %.1fs: sent_fps=%.2f sent=%lu dropped=%lu capture_errors=%lu invalid=%lu "
             "avg_jpeg=%.0f B capture=%.2f ms send=%.2f ms max_send=%.2f ms retries=%lu last_errno=%d",
             elapsed_us / 1000000.0,
             s_stats.sent * 1000000.0 / elapsed_us,
             (unsigned long)s_stats.sent,
             (unsigned long)s_stats.dropped,
             (unsigned long)s_stats.capture_errors,
             (unsigned long)s_stats.invalid,
             s_stats.sent ? (double)s_stats.sent_bytes / s_stats.sent : 0.0,
             capture_attempts ? s_stats.capture_us / (1000.0 * capture_attempts) : 0.0,
             send_attempts ? s_stats.send_us / (1000.0 * send_attempts) : 0.0,
             s_stats.max_send_us / 1000.0,
             (unsigned long)s_stats.retries,
             s_stats.last_errno);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.start_us = now_us;
}

static void pace_frame(int64_t frame_start_us)
{
    int64_t remaining_us = 1000000LL / ZHIMU_CAM_TARGET_FPS -
                           (esp_timer_get_time() - frame_start_us);
    /* Round up to RTOS ticks. Slow frames never cause a catch-up burst, and
     * even when over budget we yield so the idle/network tasks can run. */
    TickType_t ticks = 1;
    if (remaining_us > 0) {
        ticks = (TickType_t)((remaining_us * configTICK_RATE_HZ + 999999) / 1000000);
    }
    vTaskDelay(ticks);
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

    ESP_LOGI(TAG, "starting ZMC2 UDP camera stream to %s:%d", UDP_HOST, UDP_PORT);
    warmup_camera_frames();

    s_stats.start_us = esp_timer_get_time();
    while (true) {
        int64_t frame_start_us = esp_timer_get_time();
        send_frame_via_udp(sock, &dest_addr);
        log_stream_stats();
        pace_frame(frame_start_us);
    }
}
