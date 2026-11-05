#pragma once

// Copy this file to lucky_cat_private_config.h (ignored by Git).
// Set only the server's IPv4 address, without ws://, a port or a path.
// Use the test computer's LAN IP for local testing, or your cloud server IP
// when a protected remote test environment is ready. Do not use 0.0.0.0.
// Example format: "192.0.2.10" (documentation address, not a usable server).
// The test endpoint is fixed to ws://<IP>:8080/device. No TLS/auth is added.
// An empty value explicitly disables remote control, even if NVS has an URL.
#define ZHIMU_LUCKY_CAT_SERVER_IP ""

// This optional file overrides the stored NVS URL without changing NVS.
// Remove the file and rebuild to restore NVS -> menuconfig fallback behavior.
// Every change requires rebuilding and flashing the controller firmware.
