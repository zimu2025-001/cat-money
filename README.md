# cat-money · 摄像头端

`components/camera` 只保留独立摄像头工程 `esp32s3-cam/`、相关文档和主机端协议测试，不包含小智主控或中转服务的实现代码。

## 来源与分支使用

- 从 `dev/cc-fps` 的已发布提交 `f7485bbc6eab47aa3303b745f8b5b9cbfd4e5b83` 拆出，保留原 Git 历史与工程目录，不是孤立分支。
- 其他端在 `components/controller`、`components/server` 分支各自开发；完整项目基线仍可在原提交查看。
- **不要把整个单端分支直接合并回完整项目或其他端分支**：拆分提交包含删除其他端文件的操作。需要同步某项修复时，只挑选对应组件的后续提交，并检查差异。
- “只保留一端”指当前文件树；历史提交仍含原完整项目。

## 配置与构建

需要 ESP-IDF **≥5.1** 和 ESP32-S3 工具链；`main/idf_component.yml` 固定依赖 `espressif/esp32-camera` **2.1.6**。检查 OV2640 引脚和 Octal PSRAM 是否匹配实际板卡。

```sh
cd esp32s3-cam
# 将 main/camera_private_config.example.h 复制为 main/camera_private_config.h
# 填写 Wi-Fi 与中转服务的局域网 IPv4 地址，切勿提交真实凭据。
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

请将 `COMx` 替换为摄像头板实际串口。完整配置、接线与测量说明见 [摄像头文档](esp32s3-cam/README.md)。私有配置和构建产物已被 Git 忽略。

## 与其他端的连接

摄像头使用 **ZMC2 / UDP 5000** 发送 JPEG 分片，需要另外启动兼容的中转服务。协议为 22 字节网络序头部、最大 1400 字节 UDP 包、整帧 CRC32；格式见摄像头文档。设备不需要主控源码即可编译，也不直接连接主控。

`components/server` 为服务端分支；其实际启动参数和兼容性以该分支文档及测试为准。基线 Python 中转可从上述完整项目提交单独检出，用于旧实现的对照测试。默认目标 15 FPS 是发送节奏配置，不是实测性能保证。仅在可信局域网使用。

## 验证

从本分支仓库根目录运行，需 Python 3 和 C 编译器：

```sh
python -m unittest discover -s tests -v
```

默认运行独立的真实 C 编码器测试，检查 CRC、字节序、分片边界和参数校验。4 项 Python 接收器兼容测试默认跳过；若要运行，显式指定另一个可信检出目录中的基线服务端文件：

```sh
CAT_MONEY_PROXY_PATH=/absolute/path/to/full-checkout/esp32-proxy.py \
  python -m unittest discover -s tests -v
```

这会导入并执行指定的 Python 文件，只应指向可信源码。它验证的是基线 Python 接收器，不代替 Go 服务端测试。拆分验证不代表已通过 ESP-IDF 完整构建、烧录、真实 Wi-Fi 联调或硬件 FPS 测试。

## 许可

基线摄像头目录没有独立的 LICENSE 文件；本次拆分不新增或推定许可授权。第三方组件遵循各自许可证。主控目录的 MIT 许可证不自动扩展到本端。
