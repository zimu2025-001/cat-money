#pragma once

// Copy this file to camera_private_config.h and fill in your own values.
// camera_private_config.h is ignored by git and should not be committed.

#define ZHIMU_CAM_WIFI_SSID "YOUR_WIFI_SSID"
#define ZHIMU_CAM_WIFI_PASS "YOUR_WIFI_PASSWORD"
// Set only the server's IPv4 address (test computer LAN IP or cloud server IP).
// No udp://, port, path, domain name or 0.0.0.0. Rebuild and flash after changes.
#define ZHIMU_CAM_UDP_HOST "YOUR_SERVER_IP"
// Keep 5000 for the current fixed test setup; the Go server must use the same port.
#define ZHIMU_CAM_UDP_PORT 5000

// Frame pacing target, not a guarantee of sensor/network throughput (1..60).
#define ZHIMU_CAM_TARGET_FPS 15
// Drop an old frame if CRC + packet sending exceeds this budget (1..1000 ms).
#define ZHIMU_CAM_SEND_BUDGET_MS 100
