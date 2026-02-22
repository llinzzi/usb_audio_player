# USB 主机音频播放器

这是一个基于 ESP-IDF 的 USB 主机音频播放器项目，通过 USB 音频类 (UAC) 扬声器设备播放 MP3 文件。

## 功能特性

- 支持 USB UAC 1.0 扬声器设备
- MP3 文件解码与循环播放
- 自动检测设备连接/断开
- 支持 44100/48000/96000 Hz 采样率
- 支持 16/24 位音频分辨率
- 支持单声道/立体声输出

## 硬件要求

### 开发板

以下 ESP32 系列开发板均可使用：
- ESP32-S2
- ESP32-S3
- ESP32-P4

**注意**：
1. `ESP32-Sx-DevKitC` 开发板无法通过 USB 端口输出 5V 电压，如直接使用 OTG 转接线可能导致设备无法供电。
2. 对于 `esp32s3-usb-otg` 开发板，请启用 USB 主机电源域以供电给外设。

### 硬件连接

| 信号 | ESP32-S2/S3 引脚 |
|------|-----------------|
| USB_DP | GPIO20 |
| USB_DM | GPIO19 |

### 日志输出 (UART0)

本项目默认使用 UART0 输出日志（ESP32-S3）：

| 信号 | GPIO 引脚 |
|------|----------|
| TX   | GPIO43   |
| RX   | GPIO44   |
| 波特率 | 115200   |

## 构建和烧录

### 环境要求

- ESP-IDF v5.0 或更高版本（本项目使用 v5.5.2）
- ESP-IDF 路径：`/Users/zulin/esp/v5.5.2/esp-idf`

### 构建步骤

```bash
# 1. 设置 IDF 环境
. ./export.sh

# 2. 设置目标芯片 (esp32s2, esp32s3, 或 esp32p4)
idf.py set-target esp32s3

# 3. 如果组件管理器报错，升级版本
pip install "idf-component-manager~=1.1.4"

# 4. 构建、烧录和监视（替换 PORT 为实际端口）
idf.py -p PORT flash monitor
```

**常用端口**：
- macOS: `/dev/cu.usbmodemXXX`
- Linux: `/dev/ttyUSB0` 或 `/dev/ttyACM0`
- Windows: `COM3` 等

退出串口监视器：按 `Ctrl-]`

## 项目结构

```
usb_audio_player/
├── main/
│   └── usb_audio_player_main.c    # 主程序入口
├── managed_components/
│   ├── chmorgan__esp-audio-player/ # MP3 解码器和播放器抽象层
│   └── espressif__usb_host_uac/    # USB 音频类主机驱动程序
├── spiffs/
│   └── new_epic.mp3               # 默认播放的 MP3 文件
├── sdkconfig.defaults             # 项目配置文件
├── partitions.csv                 # 分区表
└── CMakeLists.txt                 # 构建配置
```

## 软件架构

### 核心组件

1. **esp-audio-player** (`managed_components/chmorgan__esp-audio-player/`)
   - MP3 解码器（基于 libhelix-mp3）
   - 音频播放器抽象层

2. **usb_host_uac** (`managed_components/espressif__usb_host_uac/`)
   - USB 音频类主机驱动程序
   - 处理 UAC 设备枚举和音频流传输

### 任务设计

主程序包含三个 FreeRTOS 任务：

| 任务名 | 优先级 | 功能 |
|--------|--------|------|
| `usb_events` | 5 | USB 主机客户端，管理设备枚举 |
| `uac_events` | 5 | UAC 驱动程序，处理扬声器连接/音频流 |
| `app_main` | 2 | 主循环，初始化 SPIFFS 和音频播放器 |

### 音频流程

```
SPIFFS (MP3 文件) → esp-audio-player (解码) → usb_host_uac → USB 扬声器
```

## 自定义 MP3 文件

如需播放自己的 MP3 文件：

1. 将 MP3 文件放入 `spiffs/` 目录
2. 修改 `main/usb_audio_player_main.c` 中的 `MP3_FILE_NAME` 宏：
   ```c
   #define MP3_FILE_NAME           "/your_file.mp3"
   ```

**推荐规格**：48 kHz, 16 位，立体声

## 默认播放文件

默认 MP3 文件为 `spiffs/new_epic.mp3`，规格为 48 kHz、16 位、立体声。

此音乐片段来自 Royalty Free Music By 500Audio，详见：https://500audio.com/track/new-epic_22682

## 日志输出示例

```
I (689) usb_audio_player: USB Host installed
I (689) usb_audio_player: UAC Class Driver installed
I (1550) usb_audio_player: UAC Device connected: SPK
find UAC 1.0 Speaker device
interface number: 2
...
I (1714) usb_audio_player: Playing '/new_epic.mp3'
```

## 相关文档

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/)
- [USB Host UAC 组件文档](managed_components/espressif__usb_host_uac/README.md)
- [ESP Audio Player 组件文档](managed_components/chmorgan__esp-audio-player/README.md)
