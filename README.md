# cat-money · 小智主控端

`components/controller` 只保留 `xiaozhi-esp32-main3/` 主控工程及其源码、配置、资源、脚本、上游文档和 MIT 许可证，不包含独立摄像头固件或中转服务实现。

## 来源与分支使用

- 从 `dev/cc-fps` 的已发布提交 `f7485bbc6eab47aa3303b745f8b5b9cbfd4e5b83` 拆出，保留原 Git 历史和工程目录，不是孤立分支。
- 其他端在 `components/camera`、`components/server` 分支各自开发；完整项目基线仍可在原提交查看。
- **不要把整个单端分支直接合并回完整项目或其他端分支**：拆分提交会删除其他端文件。只挑选需要的后续组件修复提交，并检查差异。
- “只保留一端”指当前文件树；历史提交仍含原完整项目。

## 构建与配置

需要 ESP-IDF **≥5.5.2**，以 [组件清单](xiaozhi-esp32-main3/main/idf_component.yml) 为准。项目版本 **2.2.4** 不是 ESP-IDF 版本。首次构建需要下载声明的组件依赖。

从已激活 ESP-IDF 的终端运行：

```sh
cd xiaozhi-esp32-main3
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 换成主控板串口。在 `Xiaozhi Assistant` 中确认默认板型 `Zhimu 2.25 TFT(WIFI)`、16 MB Flash 与 Octal PSRAM 设置匹配硬件。`Lucky Cat Servo Control` 默认启用，MG996R 使用 GPIO20，SG90 使用 GPIO11；旧版 UART `SG90 Servo Control` 不是这套双舵机实现。

固件源码、组件清单、默认配置和许可证在本次拆分中保持原样。主控中面向其他板型的摄像头支持仍属于主控固件，不是独立的 `esp32s3-cam/` 工程。详细资料见 [主控中文说明](xiaozhi-esp32-main3/README_zh.md)、[板型配置](xiaozhi-esp32-main3/main/boards/zhimu-2.25tft-wifi/config.h) 和 [MCP 协议](xiaozhi-esp32-main3/docs/mcp-protocol.md)。

## 与其他端的连接

语音交互使用所配置的小智服务。网页动作控制需另外启动兼容的中转服务。推荐使用下方的可编辑 IP 配置文件；也可沿用 `Remote Control WebSocket URL`，填写例如 `ws://192.168.1.10:8080/device`。最终选中的地址为空时不连接远程控制。

未使用下方私有 IP 配置时，主控优先使用 NVS 中 `lucky_cat/remote_ws_url` 的已有值，再使用编译默认值。修改 menuconfig 后若仍连接旧地址，先检查已有设备设置，不要盲目擦除 Flash。`sdkconfig.defaults.local` 虽被忽略，但当前构建脚本不会自动加载它。

### 当前测试方式：只修改配置文件中的服务器 IP

现阶段先测试已有功能，不实现身份认证、语音验证码或加密，也不要求先购买 / 部署云服务器。测试电脑的局域网 IPv4 可以先作为服务器 IP。

1. 复制 `xiaozhi-esp32-main3/main/lucky_cat_private_config.example.h` 为同目录的 `lucky_cat_private_config.h`，该私有文件已被 Git 忽略。
2. 修改这一项，只填 IPv4 地址，不填协议、端口或路径：

   ```c
   #define ZHIMU_LUCKY_CAT_SERVER_IP "192.0.2.10"
   ```

   `192.0.2.10` 仅演示格式，请换成实际测试电脑或云服务器的 IPv4。目标自动组合为 `ws://该IP:8080/device`；8080 和 `/device` 保持固定，不改小智 OTA / 语音服务的地址。
3. 执行 `idf.py build`，再向主控烧录。修改此文件不会远程更新已烧录固件。

**地址优先级：** 只要私有配置中定义了 `ZHIMU_LUCKY_CAT_SERVER_IP`，它就覆盖 NVS 与 menuconfig 的远程 URL；留空表示关闭远程控制，即使 NVS 保存了旧地址也不连接。覆盖只影响本次固件运行，**不会修改或清除 NVS、Wi-Fi 或小智激活信息**。移除私有文件并重新编译后，恢复原来的 NVS → menuconfig 行为。需要自定义端口或完整 URL 时也使用原有方式。

同一套设备的摄像头还需在摄像头分支的 `esp32s3-cam/main/camera_private_config.h` 中设置相同目标 IP、UDP 5000，并重新编译烧录。Go 服务监听 `0.0.0.0:8080` 与 UDP 5000；**不要把 `0.0.0.0` 当作设备目标地址，也不要把云公网 IP 当作 Go 的本机监听地址**。浏览器访问 `http://实际服务器IP:8080/`。

未认证、未加密的测试服务仅用于可信局域网或已隔离的测试网络，不应直接开放公网端口。

配置逻辑的主机测试：

```sh
python -m unittest discover -s tests -v
```

需要 C++17 编译器，覆盖私有配置缺失、显式覆盖、换 IP、空值关闭和旧配置回退；不需要设备，不读写实际 NVS，不替代 ESP-IDF 构建或硬件测试。

`components/server` 为服务端分支，按其文档独立启动；独立摄像头图传来自 `components/camera`。主控构建不依赖这些端的源码，默认板型也不会把独立摄像头视频自动接入大模型视觉理解。

## 安全与验证范围

- 私有配置、`sdkconfig` 和构建产物已被 Git 忽略；不要提交凭据。
- 舵机使用合适的独立电源并与主控共地，确认机械安全行程；核对传感器输出电压，避免损伤 ESP32-S3。
- 烟雾功能仅用于开发实验，不能替代合规的独立烟雾报警器。
- 本次拆分检查了文件范围、依赖引用及源码完整性；未进行 ESP-IDF 完整构建、烧录或硬件联调。原根目录的图传/中转回归测试依赖另外两端，不属于主控工程，因此不保留在本分支。
- 原工程内的 `.github/` 保留原样；它位于子目录，不能当作仓库根目录下已启用的 GitHub Actions 工作流。

## 许可

保留 [小智主控 MIT License](xiaozhi-esp32-main3/LICENSE) 及原版权声明；第三方组件遵循各自许可证。上游项目：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)。
