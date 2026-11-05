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

### 当前阶段：固定参数测试，可自行修改设备目标 IP

目前先测试已有图传和控制功能，不实施下文的身份认证、语音配对或加密计划，也不要求先提供云服务器。可先使用测试电脑的局域网 IPv4，后续再改成受保护测试环境的云服务器 IPv4。

- **Go 服务端：** 保持 HTTP / WebSocket 8080、UDP 5000，以及原有其他默认参数。直接运行 `./esp32-proxy` 即使用这些默认值。`--http-host` / `--udp-host` 的 `0.0.0.0` 是本机监听所有网卡，不是设备连接地址；云公网 IP 可能经云平台 NAT 映射，并不属于服务器本机网卡，**不要将它直接写进监听参数**。
- **摄像头分支：** 复制 `esp32s3-cam/main/camera_private_config.example.h` 为同目录的 `camera_private_config.h`，将 `ZHIMU_CAM_UDP_HOST` 改为实际服务器 IPv4，`ZHIMU_CAM_UDP_PORT` 保持 `5000`。不填协议、端口或路径；当前摄像头不解析域名。
- **主控分支：** 复制 `xiaozhi-esp32-main3/main/lucky_cat_private_config.example.h` 为同目录的 `lucky_cat_private_config.h`，将 `ZHIMU_LUCKY_CAT_SERVER_IP` 改为相同 IPv4，自动使用 `ws://该IP:8080/device`。显式私有配置覆盖已有 NVS 地址但不擦写 NVS；空值关闭远程控制；不提供该配置时保持原 NVS → menuconfig 行为。小智 OTA / 语音服务地址不受影响。
- **浏览器：** 打开 `http://实际服务器IP:8080/`，视频与控制自动使用当前网页地址，无需另改 IP。

两份设备配置属于各自固件，修改后分别重新编译并烧录才会生效；修改 Go 参数不能远程更新 ESP32 的地址。私有配置被 Git 忽略，仓库不保存真实 IP 或 Wi-Fi 密码。其余现有协议、引脚、功能和默认参数保持不变，不新增服务发现或配置下发系统。

**这里只准备本地 / 隔离网络测试，不部署云服务器、不开放公网端口，也不把明文测试服务标记为可安全公网使用。** 下文安全章节仍是后续计划。

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

## 后续计划：单设备身份认证与加密

**本节仅记录设计与实施顺序，尚未实现，也不是当前版本已具备的安全能力。** 目标是一台 **2 核 CPU / 2 GB 内存的云服务器**，只服务一套招财猫设备。现有硬件中，小智主控与摄像头是两个独立网络客户端；“单设备”按一套主控 + 摄像头理解，两者仍需独立凭据和权限。暂不做多租户、设备集群或复杂账号平台。

### 可行性与当前缺口

- **Go 转发服务 + TLS + 单设备认证适合作为这台机器的优先方案**，但不是已完成的性能保证。服务只转发 JPEG 和控制消息，不解码、转码视频，也不在这台机器部署大模型或为了播报码额外部署完整小智服务端。实际容量要结合帧大小、帧率、观看人数和云服务器带宽实测。
- 当前 HTTP / WebSocket 为明文；`/device` 不验证设备凭据，新连接会替换旧连接；网页、状态、截图、MJPEG 和动作接口没有登录保护。`Device-Id`、`Client-Id`、MAC、IP 和客户端自报的 `hello` 都只能作为标识，不能证明设备身份。
- 当前 UDP 的 ZMC2 CRC32 只能发现传输损坏，不提供加密或密码学认证；来源隔离和帧序号检查也不能阻止恶意伪造、重放。给网页加 HTTPS 不会保护另一个 UDP 端口。

### 小智语音验证码能否复用

**可以复用“设备播报码，用户在网页确认配对”的交互方式；不能把短验证码直接当作加密密钥，也不能默认把已有小智平台的激活码用于本服务。**

核对本项目主控代码后的依据：

1. [`Ota::CheckVersion()`](https://github.com/zimu2025-001/cat-money/blob/3a34450a797301f0c2fe428673127e8ad271e17b/xiaozhi-esp32-main3/main/ota.cc#L122-L143) 从 OTA 响应读取 `activation.code`、`challenge` 等字段；[`ShowActivationCode()`](https://github.com/zimu2025-001/cat-money/blob/3a34450a797301f0c2fe428673127e8ad271e17b/xiaozhi-esp32-main3/main/application.cc#L612-L641) 使用固件内的数字音频逐位播报，并非必须让云端运行 TTS 模型。后续可复用数字音频，但应新增独立的招财猫配对提示和状态流程，避免打断原小智激活流程。
2. OTA 激活还包含依赖设备预置条件的 HMAC challenge 路径，这与播出的数字码不是同一个东西。当前仓库不包含所连小智平台服务端，无法据此确认它的发码规则、有效期或提供给本服务的验证接口，也不能假定能取得或复用其 eFuse 密钥。
3. 招财猫远程控制使用独立的 [`RemoteTask()`](https://github.com/zimu2025-001/cat-money/blob/3a34450a797301f0c2fe428673127e8ad271e17b/xiaozhi-esp32-main3/main/boards/common/lucky_cat_servo_controller.cc#L190-L228)，目前只设置 `Device-Id` / `Client-Id`，没有继承小智语音协议的认证 token。因此“小智平台激活成功”不等于本 Go 服务已经认证该主控。

例如 6 位数字最多只有 100 万种组合，约 20 bit，且可能被旁人听见；哈希、补零或用 KDF 处理都不会增加原始秘密的熵。验证码只用于短时、限次的人工配对确认，真正的设备凭据另用密码学安全随机数生成，传输加密交给 TLS。短用户码与高熵设备侧凭据分离、限次及语音传码的思路可参考 [RFC 8628 第 5 节](https://datatracker.ietf.org/doc/html/rfc8628#section-5)，这里不承诺实现完整 OAuth 服务。

优先由本服务独立发配对码，保持原小智 OTA / 对话服务不变。若以后确需与自建小智服务端联动，先确认具体版本及可信的服务间验证接口，明确绑定哪个设备、哪个服务、哪次会话和有效期，再设计集成；不能只提交一串别处播出的数字便放行，也不把第三方 token 直接当作本服务的密钥。

### 阶段一：先完成可安全远程使用的最小版本

- [ ] **先建立容量边界。** 给帧缓冲增加总字节数和帧数上限，并统一 HTTP / UDP 单帧限制；设置观看连接、待认证连接、命令频率、握手和空闲时间上限。保留现有慢客户端跳帧机制。资源预算与建议验收目标见下文，不能只靠“仅一台设备”假设不会受攻击。
- [ ] **统一 HTTPS / WSS 入口。** 选择 Go 原生 TLS 或一个轻量反向代理，不重复堆叠。优先 TLS 1.3，按实际 ESP32 库兼容性保留安全配置的 TLS 1.2；使用有效域名证书，设备必须验证 CA、主机名和有效期，不允许跳过验证。固件要验证校时、证书续期和断线重连行为，参考 [ESP-TLS 服务端证书验证](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_tls.html#tls-server-verification)。
- [ ] **关闭绕过入口。** 如用同机反向代理，Go HTTP 只监听回环地址，公网不开放 8080；仅信任该代理传来的转发头。代理须支持 WebSocket Upgrade 和 MJPEG 流式响应，避免额外缓冲。公网只开放必要的 HTTPS 入口及受限管理入口；未完成摄像头安全接入前不开放原始 UDP 5000。TLS 在代理终止时，代理和服务器本身属于信任边界，不宣称云端无法看到画面。
- [ ] **先用人工配置的独立随机凭据打通认证。** 管理员、主控、摄像头分开授权；设备凭据至少使用 32 字节安全随机数，服务端可保存其摘要，凭据可撤销和轮换。只通过受保护的配置流程写入固件 / NVS，不写入公开仓库、URL 查询参数或日志；后续评估 NVS 加密等设备侧保护，不默认承诺可抵御物理提取。单设备先用权限受限的配置文件或 SQLite 即可，不引入 Redis / MySQL 集群。
- [ ] **保护全部浏览器入口。** 单管理员登录采用妥善加盐的密码哈希和限速；成功后使用 `Secure`、`HttpOnly`、`SameSite` 会话 Cookie。除登录所需资源外，面板、状态、截图、所有 MJPEG 别名、`/control` 和 `/ws` 均统一鉴权；动作请求加 CSRF 防护，浏览器 WebSocket 在升级前验证会话与精确 Origin 白名单。注销、过期、撤销时关闭对应长连接。不能只隐藏按钮或只保护首页，参考 [OWASP WebSocket Security](https://cheatsheetseries.owasp.org/cheatsheets/WebSocket_Security_Cheat_Sheet.html)。
- [ ] **主控通道单独鉴权。** 主控通过 WSS 请求头携带自己的凭据，服务端在 WebSocket 升级及替换在线连接之前验证身份与角色。仅允许已绑定主控重连；陌生连接即使伪造相同 MAC 也不能挤掉在线设备。摄像头凭据只能上传画面，不能发舵机指令，管理员 Cookie 也不当设备凭据使用。
- [ ] **先选一条摄像头安全接入路线。** 无额外网关时，优先评估修改摄像头固件，经持久 HTTPS 连接上传到已有 `/camera/frame`，使用摄像头专属凭据并测延迟 / 帧率；服务器已有 HTTP 上传接口，当前摄像头固件却只有 UDP 发送，不能只改 URL 就使用 HTTPS。如暂不改摄像头，可由可信局域网中的常开网关把 UDP 经加密 VPN / 隧道送到云端私网入口；这需要额外网关，网关前的局域网仍须可信，不能假定 ESP32 已能直接运行 VPN。

阶段一先交付“有认证、有安全传输、有资源上限”的版本；语音配对是后续便利功能，不应阻塞基础安全。没有安全视频路线时，宁可保持视频仅在可信局域网内使用，也不以 HTTPS 网页掩盖公网明文 UDP。

### 阶段二：增加独立语音配对，替代手工录入设备凭据

- [ ] 管理员先登录本服务并显式开启短暂配对窗口，同时在主控上通过实体操作进入配对模式。默认关闭配对，已绑定时不得靠再次输入验证码直接覆盖；换机 / 重置需管理员确认并撤销旧凭据。
- [ ] 主控通过已验证服务端身份的 TLS 连接申请配对。本服务返回“供用户输入的短数字码”和“仅设备保存的高熵待配对会话凭据”；只播前者。将待配对会话绑定服务、设备候选、角色和本次请求，不能用 MAC 或短码直接领取长期凭据。
- [ ] 用户在已登录的 HTTPS 管理页输入刚刚从本机听到的码，核对设备及用途后确认。建议初始规则为 **5 分钟有效、同一时刻最多一个待配对会话、每次最多 5 次验证失败**；超限使本次会话失效并冷却。创建、输入、轮询均施加全局和来源限速，计数不能通过换 IP、重新连接或服务重启绕过；重启可直接使待配对会话全部失效。
- [ ] 确认与消耗验证码必须是原子操作，成功后立即失效，拒绝过期、重复确认和旧会话重放；只有持有对应高熵会话凭据的设备，才能经 TLS 领取新生成的长期凭据。短码不派生长期密钥，也不用于后续控制认证。音频播报并不防旁听，因此仍需要管理员登录、现场确认及限次，不能包装成防窃听或强多因素认证。
- [ ] 使用主控已有数字音频播放本服务的码，不要求云端生成语音。摄像头是另一块没有播报能力的板，先保留独立的安全预配置方式；主控完成配对不自动证明摄像头身份。后续若做主控辅助摄像头配对，需先明确两块板之间可信的凭据传递路径。

### 阶段三：仅在确有必要时保留公网低延迟 UDP

如果阶段一的 HTTPS 上传或可信网关方案满足实测需求，就不另造视频加密协议。若必须让摄像头直接经公网发送 UDP：

- [ ] 先评估 Go 与实际 ESP32 / ESP-IDF 版本都可用、仍受维护的 DTLS 实现，验证握手内存、重连、分片与反重放配置，不能因为有 TLS 库就假定支持 DTLS。协议参考 [RFC 9147](https://www.rfc-editor.org/rfc/rfc9147.html)，具体版本和库在实现时再确认。
- [ ] 只有成熟方案不适用且完成安全评审后，才考虑新的带版本协议：使用成熟 AEAD（如 AES-GCM / ChaCha20-Poly1305），经已认证 TLS 控制通道建立会话密钥，对每个分片先认证再进入大块重组；将设备 / 会话标识、序号、分片位置与长度等头部绑定为认证数据。不自创密码算法，不只增加 CRC 或“token 字段”。
- [ ] 明确方向隔离、会话换钥和 nonce 唯一性：重连 / 重启必须建立新密钥或保证计数绝不复用，计数耗尽前换钥。实现有界反重放窗口，拒绝旧会话、重复包和篡改；认证前不信任报文长度或分配无界内存，限制握手 / 无效包消耗，并为认证标签和 MTU 留余量。
- [ ] 安全模式禁止自动降级到 ZMC1、ZMC2 或裸 JPEG。旧协议只在明确启用、网络隔离的兼容模式中保留。跨会话命令还需请求 ID、有效期和去重规则，避免断线重发造成重复动作；TLS 并不能替应用保证命令只执行一次，动作请求不使用 TLS 0-RTT。

### 2 核 / 2 GB 的预算与验收

以下是**实施时的初始预算 / 验收目标，不是已测性能或现有默认值**：

- 保持单 Go 进程、可选一个 TLS 代理；先限制为 1 个主控连接、1 路摄像头和最多 2 个观看连接。不做视频转码和本地 AI 推理，认证成功后不在每帧上执行昂贵的密码哈希。
- 实时观看优先；建议将回放保留改为 10–15 秒并增加约 64 MiB 的 JPEG 缓冲硬上限，同时相应降低播放延迟、起播帧数等参数。当前默认 120 秒只有时间上限：例如平均 100 KiB × 24 FPS × 120 秒，JPEG 本体就约 **281 MiB**；若每帧达到 1 MiB，则约 **2.8 GiB**，尚未计入重组、复制、TLS 和运行时开销，不能原样当作 2 GB 云主机的安全预算。
- 带宽可能先于 CPU 成为瓶颈：平均 50 KiB × 10 FPS，约 **4.1 Mbit/s**；每个同时观看的浏览器会再占一份云端出站带宽，另有协议开销。按实际上行 / 云端出站限额设置分辨率、JPEG 质量和帧率，再决定是否需要 UDP，不能只看“2 核 2 G”。
- 在目标云主机和真实 ESP32 上连续运行至少 24 小时，记录进程 RSS、CPU、实际 FPS、观看延迟、控制 ACK 延迟和重连次数。初始以 Go 服务 RSS 稳定低于 512 MiB、整机保有足够空闲内存、不持续换页、没有持续内存增长为目标；未达标先缩小缓冲 / 帧率并定位原因，不以增加安全风险换性能。
- 安全回归必须覆盖：未登录访问所有接口及别名、越权角色、伪造设备抢占、失效 / 撤销凭据、CSRF / 非法 Origin、验证码过期 / 并发兑换 / 暴力尝试、断网与重启后的重放、坏证书 / 主机名不符、视频包篡改、慢客户端和资源耗尽。检查日志不包含验证码、长期 token、会话秘密或画面内容。
- 现有 Go、固件协议与网页测试继续全部通过；新安全协议需要摄像头、主控和服务端三端联调，明确最低兼容版本与回滚方式，不能把本 README 的计划或现有回归测试视为已完成公网安全验收。
