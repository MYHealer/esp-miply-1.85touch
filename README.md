# ESP32-S3 MiPlay & DLNA 音乐投屏播放器

> 适用于 Waveshare ESP32-S3-Touch-LCD-1.85C 的音乐投屏固件，支持小米 MiPlay 和 DLNA 双协议，1.85寸 360x360 触摸屏显示封面与歌词，PCM5101A DAC 音频输出。

## 功能

### 音频
- **MiPlay 投屏** — 小米妙享/小米音箱 App 直接投屏，支持反控（上下曲/暂停/音量/Seek）
- **DLNA 投屏** — 网易云/QQ音乐/酷狗等支持 DLNA 的 App 均可投屏
- **PCM5101A DAC** — I2S 直出，无需 MCLK，硬件简洁
- **HTTP 流解码** — MP3/AAC/WAV/FLAC/OGG，512KB PSRAM 缓冲防卡顿
- **采样率自适应** — 解码器事件驱动，I2S 原子重配置
- **ALC 软音量** — 手机端/触摸端双向同步

### 显示（ST77916 360x360 QSPI 触摸屏）
- **封面显示** — 封面图实时下载 + PNG 解码 + 缩放显示
- **歌词同步** — 3 行歌词 + 逐字高亮 karaoke + 超长行自动滚动
- **触摸控制** — 上一曲/播放/下一曲 按钮，点击封面切换歌词界面
- **反控状态同步** — 手机端操作实时反映到屏幕（暂停图标/进度/曲目）

### 系统
- **自动配网** — 首次启动创建 WiFi AP，浏览器输入密码即可配网
- **Coredump** — 崩溃现场自动落 flash，便于调试
- **双协议共存** — MiPlay (mDNS) + DLNA (SSDP) 同时在线

## 硬件

### 开发板

| 项目 | 规格 |
|------|------|
| 开发板 | Waveshare ESP32-S3-Touch-LCD-1.85C |
| 主控 | ESP32-S3-WROOM-1 (N16R8) |
| Flash | 16MB |
| PSRAM | 8MB (Octal) |
| 屏幕 | ST77916 360x360 QSPI IPS |
| 触摸 | CST816S I2C (0x15) |
| 音频 | PCM5101A I2S DAC |
| I/O 扩展 | TCA9554 I2C (0x20) |

### GPIO 映射

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO_11 | I2C SDA | TCA9554 + CST816S |
| GPIO_10 | I2C SCL | TCA9554 + CST816S |
| GPIO_40 | LCD PCLK | QSPI 时钟 |
| GPIO_46 | LCD DATA0 | QSPI 数据 |
| GPIO_45 | LCD DATA1 | QSPI 数据 |
| GPIO_42 | LCD DATA2 | QSPI 数据 |
| GPIO_41 | LCD DATA3 | QSPI 数据 |
| GPIO_21 | LCD CS | QSPI 片选 |
| GPIO_18 | LCD TE | 撕裂信号 |
| GPIO_5 | LCD BL | 背光 PWM |
| GPIO_4 | Touch INT | CST816S 中断 |
| GPIO_38 | I2S LRCK | 左右声道时钟 |
| GPIO_48 | I2S SCLK | 位时钟 |
| GPIO_47 | I2S DOUT | 音频数据 |
| GPIO_14 | SD CLK | SD 卡时钟 |
| GPIO_17 | SD CMD | SD 卡命令 |
| GPIO_16 | SD D0 | SD 卡数据 |

## 快速开始

### 方式一：下载固件直接烧录

从 [Releases](https://github.com/MYHealer/esp-miply-1.85touch/releases) 下载最新固件包。

```bash
# 安装 esptool
pip install esptool

# 烧录（COM 号按实际修改）
esptool --chip esp32s3 -p COM31 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode qio --flash_size 16MB --flash_freq 80m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 miplay.bin
```

### 方式二：从源码构建

需要 ESP-IDF v5.5+ 环境。

```powershell
# 设置环境
. E:\ESP\esp_env.ps1

# 编译
cd esp_miplay-1.85c
idf.py build

# 烧录（COM 号按实际修改）
idf.py -p COM31 flash
```

## 使用步骤

1. **烧录固件** 到 ESP32-S3-Touch-LCD-1.85C
2. **首次配网** — 设备启动后创建 WiFi AP `MiPlay-XXXX`，手机连接后浏览器输入 WiFi 密码
3. **投屏播放**
   - **小米设备** — 打开小米妙享/小米音箱 App，搜索设备投屏
   - **DLNA 设备** — 打开网易云/QQ音乐，播放界面点击投屏按钮，选择设备
4. **触摸控制**

| 操作 | 效果 |
|------|------|
| 点击 上一曲 按钮 | 切换上一首 |
| 点击 播放/暂停 按钮 | 暂停/恢复播放 |
| 点击 下一曲 按钮 | 切换下一首 |
| 点击封面图 | 切换到歌词界面 |
| 歌词界面点击任意处 | 返回封面界面 |

## 技术架构

```
┌─────────────────────────────────────────────┐
│  MiPlay (TCP 55982)  │  DLNA (SSDP+SOAP)   │
│  mDNS 发现            │  UDP 1900 发现       │
│  AES-CBC 加密控制     │  明文 SOAP/XML       │
└──────────┬────────────┴──────────┬───────────┘
           │                       │
           ▼                       ▼
┌─────────────────────────────────────────────┐
│         ESP32-S3 双协议渲染器                │
│                                             │
│  Core0: WiFi + 协议 + LVGL UI (触摸)        │
│  Core1: GMF 音频管线 (解码 + ALC + I2S)     │
│                                             │
│  ┌────────────────────────────────────────┐ │
│  │  GMF: io_http → aud_dec → aud_alc     │ │
│  │       → io_codec_dev (I2S PCM5101A)   │ │
│  └────────────────────────────────────────┘ │
│                                             │
│  ┌───────────────────┐ ┌─────────────────┐  │
│  │ 360x360 触摸屏     │ │ CST816S 触控    │  │
│  │ 封面 + 歌词 + 按钮  │ │ 按钮/封面切换   │  │
│  └───────────────────┘ └─────────────────┘  │
└─────────────────────────────────────────────┘
           │
           ▼ I2S (SCLK + LRCK + DOUT)
┌─────────────────────────────────────────────┐
│            PCM5101A DAC → 耳机/功放         │
└─────────────────────────────────────────────┘
```

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v1.0 | 2026-09-05 | 初始版本：MiPlay + DLNA 双协议，360x360 触摸屏，PCM5101A 输出 |

## 许可证

MIT
