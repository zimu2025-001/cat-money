# cat-money · Go 中转服务

本分支只包含招财猫项目的 **Go 服务端、浏览器面板和服务端测试**。它接收 ESP32 摄像头画面，通过 HTTP/MJPEG 提供实时或缓冲播放，并把网页动作指令转发给小智主控。

## 三端分支

- `components/server`：本服务端，Go + 内嵌网页。
- `components/camera`：独立 ESP32-S3 / OV2640 摄像头固件，目录 `esp32s3-cam/`。
- `components/controller`：小智主控固件，目录 `xiaozhi-esp32-main3/`。

每个分支只保留对应端的代码，保留 Git 历史。原 `main` 和 `dev/cc-fps` 不会因此改变。**不要直接把这些单端分支整分支合回完整项目分支**，否则会带入删除其他端目录的变更；需要同步时选择对应提交或文件。

## 启动

需要 [Go 1.22 或更新版本](https://go.dev/dl/)。第一次构建需要网络下载锁定的 UART 依赖；构建后的二进制无需 Python、Node.js、Go 或外部网页文件。

```bash
git clone --branch components/server https://github.com/zimu2025-001/cat-money.git
cd cat-money
go build -o esp32-proxy .
./esp32-proxy --http-host 0.0.0.0 --http-port 8080 --udp-port 5000
```

Windows 可用 `go build -o esp32-proxy.exe .`，然后运行 `./esp32-proxy.exe`。开发时可用 `go run .`。Linux/macOS/Windows 的 UART 支持由 `go.bug.st/serial` 提供，版本固定在 `go.mod` / `go.sum`，不需要 `pyserial`。

打开 <http://localhost:8080/>。未连接硬件时显示离线或等待画面是正常的。默认开启 UDP，默认网页使用实时播放，也可以手动选择缓冲播放。`Ctrl+C` 或 `SIGTERM` 会停止接收器、流式响应和 WebSocket 连接。

```bash
./esp32-proxy --help
```

### 设备地址

例如服务电脑的局域网地址是 `192.168.1.10`：

- 摄像头的 UDP 目标：`192.168.1.10:5000`。
- 小智主控远程控制地址：`ws://192.168.1.10:8080/device`。
- 局域网浏览器：`http://192.168.1.10:8080/`。

设备上的 `localhost` 指设备本身，不能用来指服务电脑。固件的具体烧录、引脚和 Wi-Fi 配置请见对应单端分支的 README。

**安全范围：** 这是可信局域网内的控制服务，默认监听所有网卡。HTTP、WebSocket 和 UDP 都不加密，控制通道没有登录认证，不要直接暴露到公网。`--camera-token` 只保护 HTTP 图片上传，不保护 UDP、网页或动作控制。请使用防火墙限制 TCP 8080 和 UDP 5000，远程访问需另行配置可靠的认证及安全网络。

## 接口

| 方法 / 协议 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/`、`/index.html` | 编译进二进制的浏览器面板 |
| GET | `/status.json` | 摄像头、播放缓冲、设备及 UART 状态 |
| GET | `/snapshot.jpg` | 最新 JPEG；无画面时 503 |
| GET | `/mjpeg` | 实时 MJPEG，慢客户端跳到最新帧 |
| GET | `/mjpeg-buffered`、`/mjpeg-replay` | 延迟回放，按顺序播放，缺新帧时重复上一帧 |
| POST | `/camera/frame` | 可选 JPEG HTTP 上传 |
| POST | `/control` | JSON 动作命令 |
| WebSocket | `/ws` | 浏览器控制，连接后立即返回状态 |
| WebSocket | `/device` | 主控主动连接的设备通道 |

MJPEG 使用 `multipart/x-mixed-replace; boundary=frame`，每帧带 `Content-Type: image/jpeg` 和 `Content-Length`。停止或关闭页面会取消流，即使尚未收到第一帧也会释放连接。

### 摄像头上传

固件默认通过 UDP 发送 **ZMC2** 分片（网络字节序、CRC32）；服务同时兼容 ZMC1 和旧式裸 JPEG/分片。接收器按来源 IP + 端口隔离、拒绝重复/倒退/损坏帧，最多保存 8 个未完成帧，5 秒后淘汰。ZMC1/2 没有启动会话 ID，摄像头重启序号归零后最多可能等待 5 秒恢复。

HTTP 方式：`POST /camera/frame`，`Content-Type: image/jpeg`，必须提供有效 `Content-Length`。可选头：

- `X-Camera-Token`：配置了 token 时必须匹配。
- `X-Camera-Id`：默认 `esp32s3-cam`。
- `X-Frame-Seq`：记录到状态信息，不替代服务内部帧序号。

成功返回 `{"ok":true,"seq":1}`。无效 token 为 401，格式或图片边界错误为 400，超过 HTTP 帧大小上限为 413。上传只校验 JPEG 起止标记及长度，不对图像进行解码。

### 控制命令

支持 `up`、`down`、`left`、`right`、`stop`、`center`、`status`、`swing_on`、`swing_off`、`swing_toggle`。`stop` 在当前招财猫固件中不等于停止招手，停止招手使用 `swing_off`。

```json
{"cmd":"left","payload":{"step":5}}
```

`POST /control` 同时接受 JSON 字符串（例如 `"left"`）和对象；对象可用 `command` 代替 `cmd`。WebSocket `/ws` 接受裸文本命令或 JSON 对象。命令不区分大小写；方向命令默认步长 5；嵌套 `payload.step` 优先于顶层 `step`。

服务优先向在线设备发送 `{"type":"servo_cmd","seq":1,"cmd":"left","payload":{"step":5}}`，接受 `hello`、`servo_ack`、`servo_status` 和 `status`。ACK 按 `seq` 对应请求。新设备连接替换旧连接，并结束旧连接未完成的命令。设备未连接或发送失败时，可回退到配置的 UART；ACK 超时不会重复下发到 UART，以免动作执行两次。

UART 使用 8N1、换行分隔 JSON；发送 `type: "cmd"`，接收 `ack` / `status`。启动示例：

```bash
./esp32-proxy --serial-port /dev/ttyUSB0 --serial-baud 115200
```

## 配置兼容

Go 版本保留原 Python 服务的全部参数名称和默认值；已移除 `esp32-proxy.py`。原启动命令仅需把 `python esp32-proxy.py` 换为 `./esp32-proxy`（或 `go run .`），设备地址和协议无需改变。

| 参数 | 默认值 |
| --- | --- |
| `--http-host` / `--http-port` | `0.0.0.0` / `8080` |
| `--udp-host` / `--udp-port` | `0.0.0.0` / `5000` |
| `--serial-port` / `--serial-baud` | 未启用 / `115200` |
| `--control-step` / `--control-timeout` | `5` / `1` 秒 |
| `--packet-is-frame` | 关闭；启用后优先将一个 UDP 包当作完整 JPEG |
| `--udp-rcvbuf` / `--extractor-max-bytes` | `8388608` / `8388608` 字节 |
| `--enable-esp32-cam-udp` | 兼容保留，UDP 默认已经开启 |
| `--disable-esp32-cam-udp` | 关闭；启用后禁用 UDP，优先于 enable 参数 |
| `--camera-token` | 环境变量 `ZHIMU_CAMERA_TOKEN`，未设置时为空 |
| `--camera-max-frame-bytes` | `1048576`，仅 HTTP 上传上限 |
| `--camera-offline-after` | `10` 秒 |
| `--camera-buffer-seconds` | `120` 秒，最少保留 1 秒 |
| `--camera-playback-delay` / `--camera-playback-fps` | `30` 秒 / `24` FPS |
| `--camera-start-buffer-seconds` / `--camera-start-buffer-frames` | `30` 秒 / `120` 帧 |
| `--camera-min-playback-remaining` | `10` 秒 |

缓冲参数只影响缓冲模式和状态显示，实时 `/mjpeg` 不等待 30 秒。回放开始时选择 `now - delay` 之前的最新帧，然后顺序播放。原有摄像头状态字段与控制状态层级保持不变。

额外保护：HTTP 命令和 WebSocket 消息上限 64 KiB；请求体读取与流写入有超时；命令等待队列有上限；无效端口、负数上限或 NaN/Infinity 配置会在启动时被拒绝。视频缓冲以时间限制，内存消耗随画面大小、帧率和保留秒数增加。

## 代码结构

```text
main.go                     启动、监听、退出
config.go                   CLI 参数
frame.go / udp.go            帧缓冲、裸 JPEG、ZMC1/ZMC2 重组
http.go                     上传、截图、状态、实时/缓冲 MJPEG
control.go / websocket.go   设备与浏览器命令桥接
serial.go                   UART 兼容通道
web/index.html              内嵌网页，沿用原页面行为
*_test.go                   Go 协议、HTTP、并发与控制回归
tests/                      实际固件编码器兼容测试及网页测试
```

## 检查

```bash
go test -race -count=1 ./...
go vet ./...
go build -o esp32-proxy .
python -m unittest discover -s tests -v
node --test tests/test_camera_viewer.js
```

Go 测试不需要硬件。Python 仅用于开发测试，不是运行服务的依赖：Python 3.10+ 加 C 编译器编译 `tests/fixtures/` 中真实固件编码器，然后启动 Go 服务，经 UDP 发包、从 HTTP 获取并逐字节比对 JPEG。Node.js 18+ 测试网页实时/缓冲切换、停止、重连及页面生命周期。若已构建服务，可设 `CAT_MONEY_SERVER_BINARY` 指向该二进制来运行协议测试。

测试覆盖 ZMC1/ZMC2 CRC、乱序/丢包/重复/旧帧、来源隔离、序号回绕与重启、内存边界、HTTP token/大小/错误输入、状态、MJPEG 和空闲断开、浏览器/设备 WebSocket、命令 ACK/超时/替换连接以及模拟 UART。它们不替代真实硬件烧录、UART 驱动、摄像头 Wi-Fi 帧率或长期稳定性测试。
