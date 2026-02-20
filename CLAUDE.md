# CLAUDE.md

本文档为 Claude Code (claude.ai/code) 在此代码库中工作提供指导。

## 构建和烧录

```bash
# 设置 IDF 环境
. ./export.sh

# 设置目标芯片 (esp32s2, esp32s3, 或 esp32p4)
idf.py set-target esp32s3

# 构建、烧录和监视
idf.py -p PORT flash monitor
```

## 架构

这是一个 ESP-IDF USB 主机音频播放器，通过 USB 音频类 (UAC) 扬声器设备播放 MP3 文件。

**核心组件：**
- `main/usb_audio_player_main.c` - 主应用程序，包含三个任务：
  - `usb_lib_task` - USB 主机客户端，管理设备枚举
  - `uac_lib_task` - UAC 驱动程序，处理扬声器连接/音频流
  - `app_main` - 初始化 SPIFFS 和音频播放器
- `managed_components/chmorgan__esp-audio-player/` - MP3 解码器和播放器抽象层
- `spiffs/` - SPIFFS 分区，存放 MP3 文件（默认：`new_epic.mp3`）

**音频流程：** `esp-audio-player` 从 SPIFFS 解码 MP3 -> `usb_host_uac` 流式传输到 USB 扬声器

**配置文件：**
- `sdkconfig.defaults` - USB 主机缓冲区偏置（周期性 OUT 传输），4MB 闪存
- `partitions.csv` - 2MB SPIFFS 存储分区
- `main/idf_component.yml` - 依赖：`esp-audio-player`、`usb_host_uac`、ESP-IDF >=5.0

**硬件连接：** USB_DP 接 GPIO20，USB_DM 接 GPIO19（ESP32-S2/S3）
