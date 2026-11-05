# cat-money · 小智 AI 招财猫

基于 ESP32-S3 的语音交互与招财猫硬件项目，在[小智 ESP32](https://github.com/78/xiaozhi-esp32)的基础上，集成双舵机动作、环境传感器、门口动态检测，以及独立摄像头的网页图传。

项目由 **小智主控固件、摄像头固件、Python 中转服务** 三部分组成。默认使用两块 ESP32-S3：一块负责语音、屏幕和传感器，另一块负责摄像头采集；电脑或服务器运行中转服务，浏览器查看画面并控制招财猫。

> 摄像头图传与小智语音服务是两条独立链路。当前默认板型没有把独立摄像头的视频自动接入大模型视觉理解。

## 功能概览

- **语音交互**：沿用小智的唤醒、语音识别、对话和语音合成流程，实际能力取决于接入的服务。
- **招财猫动作**：MG996R 控制身体左右转动，SG90 控制右臂；支持方向步进、回中、持续招手，以及 MCP 和网页控制。
- **摄像头图传**：OV2640 采集 JPEG，通过 Wi-Fi UDP 分片发往中转服务，提供实时 MJPEG、缓冲播放和截图。
- **温湿度查询**：DHT11 定时采集，支持屏幕显示和 MCP 查询。
- **门口动态检测**：HC-SR04 测距并识别距离变化，触发迎宾流程。
- **烟雾报警实验**：模拟传感器读数触发屏幕提示与声音提醒，阈值需按实际硬件校准。

## 系统结构

```text
小智语音服务
    ⇅ WebSocket 或 MQTT + UDP
ESP32-S3 小智主控 ── Wi-Fi WebSocket /device ──┐
  屏幕 / 麦克风 / 扬声器 / 双舵机 / 传感器    │
                                               ⇅
ESP32-S3 摄像头 ─── JPEG 分片 / UDP 5000 ──→ esp32-proxy.py
  OV2640                                       ⇅ HTTP / WebSocket 8080
                                             浏览器
                                      视频画面 / 状态 / 动作控制
```

主控远程控制地址默认留空，需自行配置后才会连接 `/device`。两块板之间不需要通过 UART 传输摄像头画面。

### 目录导航

```text
cat-money/
├── esp32-proxy.py                    # 视频中转、网页面板、设备控制桥接
├── esp32s3-cam/                      # 独立摄像头固件
│   ├── main/main.c                  # 摄像头初始化、Wi-Fi、UDP 发送
│   ├── main/camera_private_config.example.h
│   ├── main/idf_component.yml        # 摄像头组件依赖
│   └── sdkconfig.defaults
└── xiaozhi-esp32-main3/               # 小智主控固件，项目版本 2.2.4
    ├── main/boards/zhimu-2.25tft-wifi/ # 当前默认板型、显示与传感器配置
    ├── main/boards/common/           # 舵机、传感器等通用实现
    ├── main/Kconfig.projbuild        # menuconfig 配置项
    ├── main/mcp_server.cc            # MCP 工具注册
    ├── main/idf_component.yml        # 主控组件依赖
    ├── sdkconfig.defaults
    ├── sdkconfig.defaults.esp32s3
    ├── partitions/v2/16m.csv
    └── LICENSE                      # 小智主控目录的 MIT 许可证
```

## 开始之前

### 开发环境

| 组件 | 要求与说明 |
| --- | --- |
| 小智主控固件 | ESP-IDF **≥ 5.5.2**，以其 `main/idf_component.yml` 为准 |
| 摄像头固件 | ESP-IDF **≥ 5.1**，依赖 `espressif/esp32-camera` **2.1.6** |
| Python 中转 | 建议 Python **3.10+**；默认 Wi-Fi/UDP/WebSocket 路径只使用标准库 |
| 默认主控板型 | `Zhimu 2.25 TFT(WIFI)`，ESP32-S3，16 MB Flash、Octal PSRAM 配置 |
| 摄像头板 | ESP32-S3 + OV2640，默认配置启用 Octal PSRAM，请与实际模组匹配 |

同时构建两个固件时，可使用满足主控要求的 ESP-IDF 环境。**2.2.4 是小智项目版本，不是 ESP-IDF 版本。** 首次构建会下载组件依赖，需要网络连接。

以下命令从 ESP-IDF 已激活的终端执行；`python` 应指向 Python 3。Windows 可使用 ESP-IDF PowerShell，Linux/macOS 使用已加载 ESP-IDF 环境的终端。

```bash
git clone https://github.com/zimu2025-001/cat-money.git
cd cat-money
```

### 地址约定

下文用 `192.168.1.10` 举例表示运行中转服务的电脑局域网 IPv4 地址，使用前请替换成自己的地址。

- 摄像头发送目标：`192.168.1.10:5000`，**UDP**。
- 主控远程控制地址：`ws://192.168.1.10:8080/device`。
- 浏览器面板：`http://192.168.1.10:8080/`。
- 只有在中转电脑本机访问时才使用 `localhost`；设备不能用 `localhost` 指代你的电脑。

## 快速上手

### 1. 启动中转服务

在仓库根目录运行，并保持终端开启：

```bash
python esp32-proxy.py --http-host 0.0.0.0 --http-port 8080 --udp-port 5000
```

随后在本机浏览器打开 <http://localhost:8080/>。尚未启动摄像头和主控时，面板显示离线或等待画面是正常的。

默认监听所有网络接口。请仅在可信局域网内使用，并确认防火墙允许设备访问 TCP 8080 和 UDP 5000；不要直接把这两个端口暴露到公网。

完整参数可查看：

```bash
python esp32-proxy.py --help
```

### 2. 配置并烧录摄像头

进入摄像头工程：

```bash
cd esp32s3-cam
```

将 `main/camera_private_config.example.h` 复制为同目录下的 `camera_private_config.h`，填写自己的配置：

```c
#pragma once
#define ZHIMU_CAM_WIFI_SSID "YOUR_WIFI_SSID"
#define ZHIMU_CAM_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define ZHIMU_CAM_UDP_HOST "192.168.1.10"
#define ZHIMU_CAM_UDP_PORT 5000
```

`ZHIMU_CAM_UDP_HOST` 必须是运行中转服务的 **IPv4 地址**，当前固件不解析域名。私有配置头文件已被 Git 忽略，不要将真实密码提交到仓库。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

`COM3` 只是示例。请替换为摄像头板的实际串口，如 Windows 的 `COM7`、Linux 的 `/dev/ttyUSB0` 或 macOS 的 `/dev/cu.usbmodem...`。退出串口监视器通常使用 `Ctrl+]`。

当前摄像头参数为 **VGA 640 × 480、JPEG 质量参数 12、双帧缓冲**。JPEG 按最大 1400 字节分片发送，分片间延时 20 ms；实际帧率受图像大小、网络和这些参数影响。

### 3. 配置并烧录小智主控

另开一个已激活 ESP-IDF 的终端，从仓库根目录进入主控工程：

```bash
cd xiaozhi-esp32-main3
idf.py set-target esp32s3
idf.py menuconfig
```

在 `Xiaozhi Assistant` 中核对：

1. 板型为 **Zhimu 2.25 TFT(WIFI)**，仓库默认已选择。
2. `Lucky Cat Servo Control` 已启用，MG996R 为 GPIO20，SG90 为 GPIO11。
3. 如需网页控制，在 `Remote Control WebSocket URL` 中填写 `ws://192.168.1.10:8080/device`。留空则关闭设备侧远程连接。

保存配置后构建、烧录：

```bash
idf.py build
idf.py -p COM4 flash monitor
```

将 `COM4` 替换成主控板实际串口。首次启动按屏幕、语音或串口提示完成 Wi-Fi 配网及所接入服务要求的设备激活。

配置说明：

- `menuconfig` 的结果写入本地 `sdkconfig`，此文件已被 Git 忽略。
- 仓库虽忽略 `sdkconfig.defaults.local`，但当前构建脚本**不会自动加载**这个文件；新手直接使用 `menuconfig` 即可。
- 主控会优先读取 NVS 中 `lucky_cat/remote_ws_url` 的已有值，再使用编译时默认值。若改了编译配置仍连向旧地址，请先检查已有设备配置，不要盲目擦除 Flash。
- 旧版 `SG90 Servo Control` 是另一套可选的 UART 云台实现，默认关闭；本项目默认的双舵机招财猫使用 `Lucky Cat Servo Control`，不要混用两套引脚和控制流程。

### 4. 验证整条链路

1. 打开 `http://192.168.1.10:8080/status.json`，确认 `camera.online` 为 `true`，且 `camera.seq` 持续增加。
2. 打开 `/snapshot.jpg` 检查最新画面，或直接打开 `/mjpeg` 查看实时流。
3. 确认 `control.route` 为 `device`，再在网页尝试左右转动、抬手、回中和招手。
4. 测试语音对话、温湿度查询和门口动态检测，按实际安装位置调整参数。

**网页默认使用缓冲播放**：点击网页播放按钮后，需同时积累至少 **30 秒**视频、**120 帧**，并满足播放余量条件后才会开始。低帧率时可能等待更久。需要观察更及时的画面时，直接访问 `/mjpeg`，不要把缓冲延迟误判为摄像头故障。

## 硬件接线

以下是代码中的默认引脚，**两个表分别对应两块 ESP32-S3**。相同 GPIO 编号不代表接在同一块板上；更换板型或模组后，请先核对原理图和实际接线。

### 小智主控板

来源：[板型配置](xiaozhi-esp32-main3/main/boards/zhimu-2.25tft-wifi/config.h)与[主控默认配置](xiaozhi-esp32-main3/sdkconfig.defaults)。

| 硬件 | 主控 GPIO / 配置 |
| --- | --- |
| ST7789 屏幕 | SCK 42、MOSI 41、DC 17、CS 45、RST 46、背光 47；逻辑分辨率 284 × 76 |
| I2S 麦克风 | WS 4、SCK 5、DIN 6；16 kHz 输入 |
| I2S 扬声器 | DOUT 7、BCLK 15、LRCK 16；24 kHz 输出 |
| MG996R 身体舵机 | 信号 GPIO20 |
| SG90 右臂舵机 | 信号 GPIO11 |
| DHT11 | DATA GPIO13 |
| HC-SR04 | TRIG GPIO8、ECHO GPIO9 |
| 烟雾传感器模拟输入 | GPIO1 / ADC1 CH0；代码沿用 `Mq135Sensor` 命名 |
| Boot 按键 | GPIO0 |
| 屏幕按键 | GPIO2 |
| 音量加 / 减 | GPIO40 / GPIO39 |
| 板载 LED | GPIO48 |
| 电源检测 | GPIO38 |

默认动作范围由代码限制：MG996R **20°–160°**、SG90 **30°–150°**，回中均为 **90°**，持续招手使用 **70°–120°**。首次调试应留出机械活动空间，并按实际机构确认方向和安全行程。

### 摄像头板

来源：[摄像头初始化配置](esp32s3-cam/main/main.c)。

| OV2640 信号 | 摄像头板 GPIO |
| --- | --- |
| XCLK | 15 |
| SCCB SDA / SCL | 4 / 5 |
| D0 / D1 / D2 / D3 | 11 / 9 / 8 / 10 |
| D4 / D5 / D6 / D7 | 12 / 18 / 17 / 16 |
| VSYNC / HREF / PCLK | 6 / 7 / 13 |
| PWDN / RESET | `-1`，代码中未使用 GPIO 控制 |

接线与供电注意：

- 舵机使用满足其工作电流需求的电源，与主控共地，不要用 GPIO 给舵机供电。
- 核对 HC-SR04 ECHO 和烟雾模块 AO 的输出电压，必要时使用分压或电平转换，避免超出 ESP32-S3 输入范围。
- 烟雾功能仅用于开发实验，不能替代独立、合规的烟雾报警设备。

## 控制与接口

### HTTP / WebSocket

以下路径均由 `esp32-proxy.py` 提供，默认端口为 8080。

| 方法 / 协议 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/` | 网页面板 |
| GET | `/status.json` | 摄像头、缓冲区、设备控制连接状态 |
| GET | `/snapshot.jpg` | 最新 JPEG；还未收到画面时返回 503 |
| GET | `/mjpeg` | 实时 MJPEG |
| GET | `/mjpeg-buffered` | 缓冲播放，默认延迟参数为 30 秒 |
| GET | `/mjpeg-replay` | 缓冲播放的别名 |
| POST | `/control` | JSON 动作指令 |
| WebSocket | `/ws` | 浏览器控制连接 |
| WebSocket | `/device` | 小智主控主动连接的设备通道 |
| POST | `/camera/frame` | 可选的 JPEG HTTP 上传接口；仓库摄像头固件默认仍走 UDP |

`POST /control` 示例请求体：

```json
{"cmd":"left","payload":{"step":5}}
```

支持 `left`、`right`、`up`、`down`、`center`、`status`、`swing_on`、`swing_off`、`swing_toggle`。接口也接受 `stop`，但当前招财猫实现不会用它关闭持续招手；停止招手请使用 `swing_off`。

中转服务保留 `--serial-port` / `--serial-baud`，用于旧版 UART 控制桥接；只有使用这条兼容路径时才需要安装 `pyserial`，它不是默认招财猫连接方式。

### MCP 与按键

默认主控固件提供：

- `self.servo.move`：方向、回中、招手控制。
- `self.servo.set_angle`：设置 MG996R 和 SG90 角度，实际输出受安全范围约束。
- `self.servo.get_status`：读取舵机状态。
- 温湿度工具：`self.environment.get_temperature`、`self.environment.get_humidity`、`self.environment.get_weather`，其中 `get_weather` 返回本地传感器信息，并非互联网天气预报。

主控默认按键行为：

| 按键 | 操作 |
| --- | --- |
| Boot 单击 / 长按 / 双击 | 切换界面（启动阶段单击进入配网）/ 进入配网 / 烟雾报警测试 |
| 屏幕键单击 / 长按 / 双击 | 切换界面 / 进入配网 / 开关招手 |
| 音量键单击 | 音量增减 10 |
| 音量加键 / 减键长按 | 最大音量 / 静音 |

## 常见问题

| 现象 | 优先检查 |
| --- | --- |
| 找不到 `idf.py` | 是否使用已激活的 ESP-IDF 终端 |
| 主控依赖解析失败 | ESP-IDF 是否满足 ≥5.5.2，组件下载网络是否可用 |
| 摄像头始终离线 | 私有配置是否填写、Wi-Fi 是否连上、中转 IPv4 是否正确、UDP 5000 是否可达 |
| 截图正常，网页仍在等画面 | 缓冲是否同时达到 30 秒和 120 帧；先用 `/mjpeg` 验证实时流 |
| 有画面但按钮无效 | 主控 `/device` 地址是否配置、主控是否联网、`control.route` 是否为 `device` |
| 修改默认配置后未生效 | 已有 `sdkconfig` 或 NVS 是否覆盖新默认值 |
| 舵机动作时重启或抖动 | 电源容量、共地、接线、机械卡阻与行程限制 |
| 烟雾报警不灵敏或误报 | 查看串口 `Smoke sensor: raw=...`，在板型配置中校准阈值、回差和报警方向 |

烟雾配置当前默认阈值为 **1800**，回差 **180**，约每 **3 秒**读取一次。这些数值是 ADC 原始读数阈值，不是经过标定的气体浓度。

## 配置与安全

- 真实 Wi-Fi 密码、设备凭据及私有地址放在本地配置中，提交前检查 `git diff --cached`。
- 已忽略 `camera_private_config.h`、`sdkconfig`、`sdkconfig.defaults.local`、构建产物等文件；不要用强制添加绕过忽略规则上传私密配置。
- 中转服务当前没有为网页、视频和控制接口提供完整身份认证，不应直接公开部署。
- `--camera-token` 或环境变量 `ZHIMU_CAMERA_TOKEN` 仅保护 `POST /camera/frame`，上传方需发送 `X-Camera-Token`。它**不保护 UDP 图传、视频读取、网页控制或设备 WebSocket**。
- 默认服务按单路摄像头、单个主控连接组织；多个设备共用实例可能覆盖画面或替换控制连接，需要多设备时应先设计隔离方式。

## 参考与许可

- [小智 ESP32 上游项目](https://github.com/78/xiaozhi-esp32)
- [主控工程中文说明](xiaozhi-esp32-main3/README_zh.md)
- [摄像头工程说明](esp32s3-cam/README.md)
- [MCP 协议说明](xiaozhi-esp32-main3/docs/mcp-protocol.md)
- [自定义板型文档](xiaozhi-esp32-main3/docs/custom-board.md)

小智主控目录附带 [MIT License](xiaozhi-esp32-main3/LICENSE)。使用或分发该部分代码时请保留相应版权和许可声明；第三方组件遵循各自许可证，其他目录请以其实际许可声明为准。
