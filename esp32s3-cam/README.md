# ESP32-S3 Camera UDP Stream

This project has been migrated from PlatformIO to a standard ESP-IDF layout.

## Project Layout

- `main/main.c`: application entry and camera/Wi-Fi/UDP logic
- `main/CMakeLists.txt`: main component build file
- `main/idf_component.yml`: managed component dependencies
- `sdkconfig.defaults`: default ESP32-S3 PSRAM settings
- `GIT_TUTORIAL.md`: Git notes based on this repository

## Build Steps

1. Install ESP-IDF on this computer.
2. Open the ESP-IDF PowerShell environment.
3. Copy `main/camera_private_config.example.h` to `main/camera_private_config.h`,
   then fill in your own Wi-Fi and server settings. The private file is ignored
   by git.
4. Run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## Important Note

`idf.py` is not currently available in this terminal session, so the project structure
has been migrated but not compiled on this machine yet.

If you previously generated a wrong `sdkconfig` or `build/` directory with target `esp32`,
delete them first or run `idf.py fullclean`, then run `idf.py set-target esp32s3` again.

## Private Configuration

Do not put real Wi-Fi passwords or public server IPs in committed source files.

```c
#define ZHIMU_CAM_WIFI_SSID "YOUR_WIFI_SSID"
#define ZHIMU_CAM_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define ZHIMU_CAM_UDP_HOST "YOUR_SERVER_IP"
#define ZHIMU_CAM_UDP_PORT 5000
```
