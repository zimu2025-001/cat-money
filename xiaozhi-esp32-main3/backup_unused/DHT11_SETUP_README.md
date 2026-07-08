# DHT11/DHT22 温湿度传感器配置指南

## 功能说明

本项目已集成 DHT11/DHT22 温湿度传感器支持，通过 ESP32-S3 控制 DHT11 模块获取当前环境的温湿度，并可通过小智 MCP 协议进行语音查询。

### 支持的语音指令示例：
- "小智，告诉我今天的温湿度情况"
- "小智，现在多少度？"
- "小智，当前湿度是多少？"
- "小智，房间里的温度和湿度怎么样？"

## 硬件连接

### DHT11 引脚定义：
```
DHT11 模块    ESP32-S3
-----------  ----------
VCC          3.3V 或 5V
DATA         GPIO 引脚 (例如：GPIO8)
GND          GND
```

### 推荐电路：
- 在 VCC 和 DATA 之间添加一个 4.7kΩ-10kΩ 的上拉电阻
- DATA 引脚直接连接到 ESP32-S3 的任意 GPIO

## 软件配置

### 步骤 1: 在板级配置文件中添加 DHT11 支持

在你的板子目录中（例如 `main/boards/bread-compact-wifi/`），编辑 `config.h` 文件：

```cpp
// 添加 DHT11 配置
#define DHT11_PIN GPIO_NUM_8      // 设置 DHT11 数据引脚
#define DHT11_TYPE DHT11          // 传感器类型：DHT11 或 DHT22
```

### 步骤 2: 修改板级实现

在你的板子 `.cc` 文件中添加 DHT11 支持：

```cpp
#include "boards/common/dht11.h"

// 在板类中添加 DHT11 成员
class YourBoard : public WifiBoard {
private:
    DHT11* dht11_ = nullptr;

public:
    YourBoard() {
        // 初始化 DHT11
        dht11_ = new DHT11(DHT11_PIN, DHT11_TYPE == DHT22);
        dht11_->Init();
    }

    virtual DHT11* GetDHT11() override {
        return dht11_;
    }
};
```

### 步骤 3: 编译和烧录

```bash
# 进入项目目录
cd xiaozhi-esp32-main

# 设置目标芯片 (如果是 ESP32-S3)
idf.py set-target esp32s3

# 编译并烧录
idf.py build flash monitor
```

## MCP 工具说明

系统会自动注册以下 MCP 工具（当检测到 DHT11 硬件时）：

### 1. `self.environment.get_weather`
获取温湿度综合信息（推荐）
- 用途：当用户询问天气、温度或湿度时使用
- 返回：温度和湿度的综合信息

### 2. `self.environment.get_temperature`
仅获取温度信息
- 用途：当用户只询问温度时使用
- 返回：当前温度值

### 3. `self.environment.get_humidity`
仅获取湿度信息
- 用途：当用户只询问湿度时使用
- 返回：当前湿度值

## 注意事项

1. **读取间隔**：DHT11 传感器两次读取之间至少需要间隔 2 秒，驱动会自动处理这个限制
2. **引脚选择**：避免使用 GPIO6-11（这些是 Flash 引脚），推荐使用 GPIO1-19 中的任意引脚
3. **传感器类型**：代码支持 DHT11 和 DHT22，DHT22 精度更高，测量范围更广
4. **数据有效性**：驱动会缓存最近 2 秒内的读数，避免频繁读取传感器

## 故障排查

### 问题：读取超时
- 检查接线是否正确
- 确认上拉电阻已连接
- 尝试更换 GPIO 引脚

### 问题：校验和错误
- 检查电源电压是否稳定
- 确保接线牢固
- 添加去耦电容

### 问题：数据不变
- 确认传感器工作环境正常
- DHT11 精度有限，小幅变化可能无法检测

## 示例完整代码

参考 `main/boards/bread-compact-wifi/compact_wifi_board.cc` 进行修改：

```cpp
#include "wifi_board.h"
#include "boards/common/dht11.h"
// ... 其他头文件

class CompactWifiBoard : public WifiBoard {
private:
    DHT11* dht11_ = nullptr;
    // ... 其他成员

public:
    CompactWifiBoard() : /* 初始化列表 */ {
        // ... 其他初始化
        
        // 初始化 DHT11 传感器
        dht11_ = new DHT11(GPIO_NUM_8, false);  // false = DHT11, true = DHT22
        dht11_->Init();
    }

    virtual DHT11* GetDHT11() override {
        return dht11_;
    }
    
    // ... 其他方法
};
```

## 技术规格

| 参数 | DHT11 | DHT22 |
|------|-------|-------|
| 温度范围 | 0-50°C | -40-80°C |
| 温度精度 | ±2°C | ±0.5°C |
| 湿度范围 | 20-90% | 0-100% |
| 湿度精度 | ±5% | ±2% |
| 采样率 | 1Hz | 0.5Hz |
