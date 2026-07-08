# 小智 AI 摄像头（xiaozhi-cam）

基于 ESP32-S3 的 AI 语音交互 + 视觉 + 多传感器智能设备。在 [小智 AI 聊天机器人](https://github.com/78/xiaozhi-esp32) 的基础上，增加了摄像头图传、舵机云台、温湿度、超声波门磁、烟雾报警等功能，实现"让 AI 看见世界、感知环境、控制硬件"。

## 硬件清单

| 硬件 | GPIO | 说明 |
|------|------|------|
| **ESP32-S3** (16MB Flash, Octal PSRAM) | — | 主控芯片 |
| **OV2640 摄像头** | XCLK=15, D0-D7=11,9,8,10,12,18,17,16 | JPEG 图像采集，HVGA 480×320 |
| **ST7789 TFT 屏幕** | SCL=42, SDA=41, DC=17, CS=45, RST=46, BL=47 | 284×76 长条形显示屏 |
| **I2S MEMS 麦克风** | WS=4, SCK=5, DIN=6 | 16kHz 语音输入 |
| **I2S 扬声器** | DOUT=7, BCLK=15, LRCK=16 | 24kHz 语音输出 |
| **DHT11 温湿度传感器** | DATA=13 | 温度 + 湿度采集，每 10s 刷新 |
| **HC-SR04 超声波传感器** | TRIG=8, ECHO=9 | 动态移动检测，进店触发小智迎宾 |
| **SG90 舵机** | SIG=18 | 云台旋转控制，0~180° |
| **MQ-2 烟雾报警传感器** | AO=ADC1_CH0 (GPIO1) | 烟雾状态检测，触发后屏幕警报并播放提示音 |
| **Boot 按键** | GPIO0 | 单击切换对话 / 长按进入配网 |
| **Vol+ / Vol-** | GPIO40 / GPIO39 | 音量加减 |
| **电源检测** | GPIO38 (ADC) | 电池电量 / 充电状态 |
| **深度睡眠** | GPIO21 (RTC) | 低功耗待机唤醒 |

## 系统架构

```
┌─────────────────────────────┐
│       小智 AI 云端           │
│   (xiaozhi.me / 自部署)      │
│   Qwen / DeepSeek 大模型     │
│   ASR 语音识别 + TTS 合成    │
└─────────────┬───────────────┘
              │ WebSocket / MQTT+UDP
              │ OPUS 音频流 + MCP 设备指令
┌─────────────┴───────────────┐
│   ESP32-S3 主控 (小智固件)   │
│   ├─ 语音交互（唤醒词/AEC）   │
│   ├─ MCP 硬件控制协议        │
│   ├─ DHT11 / MQ-2 烟雾报警  │
│   ├─ HC-SR04 门磁检测       │
│   └─ SG90 舵机驱动          │
└──────┬──────────────────────┘
       │ UART (115200) - 舵机控制指令
       │
┌──────┴──────────┐    UDP (5000)    ┌─────────────────┐
│  ESP32-S3 摄像头 │ ── JPEG 分片 ──▶ │  esp32-proxy.py  │
│  (esp32s3-cam)  │                  │  ├─ MJPEG 流     │
│  OV2640 采集     │                  │  ├─ WebSocket    │
└─────────────────┘                  │  └─ HTTP 控制    │
                                     └────────┬────────┘
                                              │ HTTP :8080
                                     ┌────────┴────────┐
                                     │  浏览器 / 客户端  │
                                     │  实时画面 + 云台  │
                                     │  方向控制面板     │
                                     └─────────────────┘
```

## 项目结构

```
xiaozhi-cam/
├── esp32-proxy.py              # Python 中转服务：UDP 收图 → HTTP MJPEG + WebSocket 云台控制
├── esp32s3-cam/                # 摄像头固件（ESP-IDF）
│   ├── main/
│   │   └── main.c              # OV2640 采集 → UDP 分片发送
│   ├── CMakeLists.txt
│   └── sdkconfig.defaults
├── xiaozhi-esp32-main3/        # 小智 AI 主固件（ESP-IDF v2.2.4）
│   ├── main/
│   │   ├── main.cc             # 入口：初始化传感器 → 启动 Application
│   │   ├── application.cc      # 核心状态机：从配网到对话的完整流程
│   │   ├── mcp_server.h        # MCP 协议服务端：注册硬件控制工具
│   │   └── boards/
│   │       ├── common/         # 通用驱动
│   │       │   ├── dht11.h/cc          # 温湿度传感器
│   │       │   ├── hc_sr04.h/cc        # 超声波测距
│   │       │   ├── sg90_servo.h/cc     # 舵机控制
│   │       │   ├── door_monitor.h/cc   # 门磁检测
│   │       │   └── mq135.cc            # MQ-2 模拟量采集 / 烟雾报警
│   │       └── zhimu-2.25tft-wifi/          # 目标板卡配置
│   ├── sdkconfig.defaults              # SDK 配置
│   └── partitions/v2/16m.csv          # 16MB 分区表
└── .gitignore                   # Git 忽略规则
```

## 三大组件详解

### 1. esp32-proxy.py — 视频中转 & 云台控制

UDP 接收摄像头 JPEG 分片 → 拼接完整帧 → 提供多种 HTTP 访问方式。

**启动方式：**
```bash
python esp32-proxy.py
```

**服务端点：**

| 端点 | 说明 |
|------|------|
| `http://localhost:8080/` | 内置 Web 控制面板，实时 MJPEG 画面 + 方向按钮 |
| `http://localhost:8080/mjpeg` | MJPEG 视频流 |
| `http://localhost:8080/snapshot.jpg` | 单帧截图 |
| `http://localhost:8080/status.json` | 设备在线状态、帧序号 |
| `ws://localhost:8080/ws` | WebSocket，发送方向指令控制云台 |

**常用启动方式：**
```bash
python esp32-proxy.py --http-host 0.0.0.0 --http-port 8080 --udp-port 5000
```

如需启用摄像头 HTTP 上传 token，请通过环境变量或启动参数设置，不要写入源码：

```bash
export ZHIMU_CAMERA_TOKEN="your-camera-token"
python esp32-proxy.py --camera-token "$ZHIMU_CAMERA_TOKEN"
```

### 2. esp32s3-cam — 摄像头固件

独立的摄像头采集固件，采集 OV2640 画面并通过 Wi-Fi UDP 发送到中转服务。

**构建 & 烧录：**
```bash
cd esp32s3-cam
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**依赖：** ESP-IDF >= 5.1，`espressif/esp32-camera:2.1.6`

**注意事项：**
- 复制 `main/camera_private_config.example.h` 为 `main/camera_private_config.h`，填写本地 Wi-Fi 和服务器地址；该私有文件已被 `.gitignore` 忽略
- 相机默认采集 HVGA (480×320)，JPEG 质量 12
- JPEG 帧通过 1400 字节 UDP 分片发送，接收端由 proxy 自动拼接

### 3. xiaozhi-esp32-main — 小智 AI 主固件

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.2.4，深度定制了以下功能：

**传感器集成（MCP 协议）：**
- **DHT11** — `self.environment.get_temperature` / `get_humidity` / `get_weather`
- **MQ-2 烟雾报警** — GPIO1 读取 AO 模拟量，超过阈值后屏幕警报并播放提示音；阈值可根据串口日志 `Smoke sensor: raw=...` 现场校准
- **HC-SR04** — 动态移动迎宾，进店触发小智财运祝福
- **SG90 舵机** — `self.servo.set_angle` / `swing` / `reset` / `get_status`

**语音控制示例：**
- "小智小智，把摄像头转到左边"
- "小智，当前温度多少"
- "小智，扫描一下房间"

**构建 & 烧录：**
```bash
cd xiaozhi-esp32-main3
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

首次启动会进入配网模式，屏幕显示二维码，用手机扫码配置 Wi-Fi。

远程舵机 WebSocket 默认关闭。公开代码中 `CONFIG_LUCKY_CAT_REMOTE_WS_URL` 为空，需要远程遥控时请在本地 `idf.py menuconfig` 或私有 `sdkconfig.defaults.local` 中设置自己的服务器地址。

## 完整工作流

1. **烧录固件** — 分别在两块 ESP32-S3 上烧录 `esp32s3-cam` 和 `xiaozhi-esp32-main`
2. **启动中转** — 在电脑上运行 `python esp32-proxy.py`
3. **配网** — 小智设备首次启动，手机扫码配网
4. **开始使用** — 打开浏览器 `http://localhost:8080` 查看实时画面，说"小智小智"唤醒对话

## 开发环境

- **ESP-IDF**: v5.1+
- **Python**: 3.8+
- **目标芯片**: ESP32-S3 (16MB Flash, Octal PSRAM)
- **编译器**: xtensa-esp32s3-elf-gcc (riscv 变体使用 riscv32-esp-elf-gcc)

## 参考资料

- [小智 ESP32 原项目](https://github.com/78/xiaozhi-esp32)
- [ESP32-Camera 组件文档](https://docs.espressif.com/projects/esp32-camera)
- 传感器规格书见 `xiaozhi-esp32-main3/` 目录下 PDF 文件

## License

本项目基于 [小智 ESP32](https://github.com/78/xiaozhi-esp32) 修改，遵循原项目 MIT 许可证。
