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

语音交互使用所配置的小智服务。网页动作控制需另外启动兼容的中转服务，并在 `Remote Control WebSocket URL` 中填写例如 `ws://192.168.1.10:8080/device`。地址留空则不连接远程控制。

主控优先使用 NVS 中 `lucky_cat/remote_ws_url` 的已有值，再使用编译默认值。修改配置后若仍连接旧地址，先检查已有设备设置，不要盲目擦除 Flash。`sdkconfig.defaults.local` 虽被忽略，但当前构建脚本不会自动加载它。

`components/server` 为服务端分支，按其文档独立启动；独立摄像头图传来自 `components/camera`。主控构建不依赖这些端的源码，默认板型也不会把独立摄像头视频自动接入大模型视觉理解。

## 安全与验证范围

- 私有配置、`sdkconfig` 和构建产物已被 Git 忽略；不要提交凭据。
- 舵机使用合适的独立电源并与主控共地，确认机械安全行程；核对传感器输出电压，避免损伤 ESP32-S3。
- 烟雾功能仅用于开发实验，不能替代合规的独立烟雾报警器。
- 本次拆分检查了文件范围、依赖引用及源码完整性；未进行 ESP-IDF 完整构建、烧录或硬件联调。原根目录的图传/中转回归测试依赖另外两端，不属于主控工程，因此不保留在本分支。
- 原工程内的 `.github/` 保留原样；它位于子目录，不能当作仓库根目录下已启用的 GitHub Actions 工作流。

## 许可

保留 [小智主控 MIT License](xiaozhi-esp32-main3/LICENSE) 及原版权声明；第三方组件遵循各自许可证。上游项目：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)。
