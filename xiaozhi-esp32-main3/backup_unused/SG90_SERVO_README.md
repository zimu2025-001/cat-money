# SG90 舵机语音控制集成说明

## 功能概述

本模块将 SG90 舵机控制集成到小智 ESP32 项目中，通过 MCP 协议实现语音控制舵机转动。

## 硬件连接

```
SG90 舵机     →    ESP32N1 (或其他 ESP32-S3 开发板)
────────────────────────────────────────────
棕色线 (GND)  →    GND
红色线 (VCC)  →    5V (或 3.3V，根据舵机规格)
橙色线 (SIG)  →    GPIO21 (可配置)
```

**注意：**
- SG90 舵机工作电流约 200-300mA，建议使用外部 5V 电源供电
- 确保 ESP32 和舵机的 GND 共地
- 不同开发板的可用 PWM GPIO 引脚不同，GPIO48 在某些板子上是特殊功能引脚

## 编译配置

### 方法一：使用 menuconfig

```bash
cd xiaozhi-esp32-main
idf.py menuconfig
```

导航到：
```
Xiaozhi Assistant → SG90 Servo Control
  [*] Enable SG90 Servo Control
  (21) SG90 Servo GPIO Pin
```

### 方法二：修改 sdkconfig

在 `sdkconfig` 文件中添加：
```
CONFIG_ENABLE_SG90_SERVO=y
CONFIG_SG90_SERVO_GPIO=21
```

## 编译烧录

```bash
# 清理并重新编译
idf.py fullclean
idf.py build

# 烧录（替换 COMx 为你的实际端口）
idf.py -p COMx flash monitor
```

## 语音控制命令

连接小智服务器后，可以通过以下语音命令控制舵机：

### 1. 设置舵机角度
```
"小智，让舵机转到 90 度"
"小智，舵机转到 45 度"
"小智，让舵机转到 180 度"
```

### 2. 查询当前角度
```
"小智，舵机现在多少度"
"小智，舵机在什么位置"
```

### 3. 舵机复位
```
"小智，舵机复位"
"小智，舵机回到 0 度"
```

### 4. 摆动测试
```
"小智，舵机摆动一次"
"小智，让舵机摆动 3 次"
```

### 5. 查询状态
```
"小智，舵机状态"
```

## MCP 工具接口

### self.servo.set_angle
设置舵机角度（0-180 度）
```json
{
    "name": "self.servo.set_angle",
    "arguments": {"angle": 90}
}
```

### self.servo.get_angle
获取当前舵机角度
```json
{
    "name": "self.servo.get_angle",
    "arguments": {}
}
```

### self.servo.reset
舵机复位到 0 度
```json
{
    "name": "self.servo.reset",
    "arguments": {}
}
```

### self.servo.swing
舵机摆动测试
```json
{
    "name": "self.servo.swing",
    "arguments": {"times": 1}
}
```

### self.servo.get_status
获取舵机状态信息
```json
{
    "name": "self.servo.get_status",
    "arguments": {}
}
```

## 修改舵机 GPIO 引脚

如果需要使用其他 GPIO 引脚，在 menuconfig 中修改，或直接在 `sdkconfig` 中设置：

```
CONFIG_SG90_SERVO_GPIO=47  # 例如改为 GPIO47
```

**ESP32-S3 可用的 PWM GPIO 引脚：**
- 推荐：GPIO1-21, GPIO33-47
- 避免：GPIO48-50（USB 相关特殊引脚）

## 故障排查

### 1. 舵机不转动
- 检查供电：舵机 VCC 是否有 5V 电压
- 检查接线：GND 是否共地，SIG 是否连接到正确的 GPIO
- 检查 GPIO：某些 GPIO 引脚可能有特殊功能，尝试更换引脚
- 查看日志：串口日志是否有错误信息

### 2. 编译错误
- 确保已启用 `CONFIG_ENABLE_SG90_SERVO=y`
- 检查 `sdkconfig` 中是否有正确的 GPIO 配置
- 清理并重新编译：`idf.py fullclean && idf.py build`

### 3. 语音控制无响应
- 确认小智已连接到服务器
- 检查 MCP 工具是否正确注册（查看启动日志）
- 确认语音指令格式正确

## 文件清单

```
main/
├── boards/common/
│   ├── sg90_servo.h              # SG90 舵机驱动头文件
│   └── sg90_servo_controller.cc  # MCP 舵机控制器实现
├── main.cc                       # 主程序（已添加初始化调用）
├── CMakeLists.txt                # 构建配置（已添加源文件）
└── Kconfig.projbuild             # 配置菜单（已添加选项）
```

## 扩展开发

可以基于此模块添加更多功能：
- 多舵机控制（需要多个 GPIO 引脚）
- 舵机动作序列（预定义动作组合）
- 与其他传感器联动（如语音 + 手势控制）

## 注意事项

1. **供电**：SG90 舵机瞬时电流较大，建议使用独立 5V 电源
2. **GPIO 选择**：避免使用具有特殊功能的 GPIO 引脚
3. **角度范围**：SG90 标准舵机角度范围为 0-180 度
4. **响应时间**：舵机转动需要时间，连续命令之间应间隔至少 500ms

## 技术支持

如有问题，请参考小智 ESP32 项目文档或提交 Issue。
