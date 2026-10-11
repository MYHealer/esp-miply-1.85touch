# ESP32-S3-Touch-LCD-1.85C — MiPlay / DLNA 投屏播放器

> 基于 ESP-GMF 的 WiFi 音频投屏渲染器，运行在 **ESP32-S3-Touch-LCD-1.85C 一体化开发板**上。
> 支持 MiPlay（小米妙播）、DLNA 投屏，1.85 寸 360×360 圆形 QSPI 屏显示唱片动画与歌词，电容触摸 + 手机遥控双向控制。

**当前版本：v1.5.0**

## 硬件平台

本项目针对 **ESP32-S3-Touch-LCD-1.85C** 一体化开发板编写，不是手工接线的方案。

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3（Xtensa 双核，revision v0.2） |
| 内存 | 16MB Flash + 8MB Octal PSRAM |
| 屏幕 | **ST77916** 1.85" 圆形 TFT，**360×360**，**QSPI** 接口（不是 SPI） |
| 触摸 | **CST816S** 电容触摸（I2C 地址 `0x15`） |
| I/O 扩展 | **TCA9554** 8-bit 扩展器（I2C `0x20`） |
| 音频输出 | 板载 I2S 功放 |
| 电池 | 板载锂电池 + 充电检测（ADC1_CH7 / GPIO8） |

> ⚠️ **不要照搬网上 1.8" ST7735S + PCM5102A 的接线方案。** 本项目屏幕是 QSPI 的 ST77916，
> 分辨率、接口、引脚都与 ST7735S 完全不同。手工接线方案无法运行本固件。

### 引脚（来自板级定义，勿凭记忆修改）

| 用途 | GPIO | 定义位置 |
|------|------|---------|
| I2S LRCK | 38 | `components/i2s_pa/include/board_def.h:11` |
| I2S SCLK | 48 | `components/i2s_pa/include/board_def.h:12` |
| I2S DOUT | 47 | `components/i2s_pa/include/board_def.h:13` |
| 触摸 INT | 4 | `components/lvgl_port/lvgl_port_touch.c:19` |
| I2C SDA | 11 | `components/echopal_board/include/echopal_board.h` |
| I2C SCL | 10 | `components/echopal_board/include/echopal_board.h` |
| 电池 ADC | 8 | `components/bat_monitor/bat_monitor.c:14` |

屏幕与触摸的复位脚不在 GPIO 上，而是挂在 TCA9554 扩展器：`EXIO1` = 触摸复位，`EXIO2` = LCD 复位（低有效）。

## 功能

### 投屏协议
- ✅ **MiPlay（小米妙播）** — 协议级对接，支持反向控制（手机端播放/暂停/切歌/Seek/音量同步到设备）
- ✅ **DLNA** — 网易云/QQ音乐/酷狗等 DLNA App 投屏
- ✅ **airkan 遥控** — 小米妙享遥控器方向键 Seek、OK 键播放暂停、关机键切回屏保
- ✅ **GENA 事件同步** — 手机端实时显示播放状态，歌曲自动切换

### 音频
- ✅ **GMF 音频管线** — `io_http → aud_dec → aud_alc → io_i2s`
- ✅ **多编码格式** — MP3 / AAC / WAV / FLAC / OGG
- ✅ **采样率/位深自适应** — 解码器事件驱动，I2S 原子重配置，支持 16/24-bit
- ✅ **双管线音量统一** — DLNA 与 MiPlay 各自的 ALC 音量同步一致
- ✅ **提示音**（`components/chime`）— 充电接入等事件音效

### 显示（ST77916 360×360）
- ✅ **唱片旋转动画** — 封面圆形裁剪 + 匀速旋转，唱针 ease-out 落针
- ✅ **动态背景** — 从封面提取主色，Bayer 4×4 有序抖动渐变（消除 RGB565 色阶）
- ✅ **歌词同步** — 多行歌词 + 逐字高亮 karaoke + 超长行自动左移
- ✅ **中文字体** — SimSun 16px 全字库 + 补充字集（编译进固件）
- ✅ **电量/充电图标** — 状态栏显示，充电时动画提示
- ✅ **2 分钟锁屏** — 非播放态无操作 2 分钟进入黑底时钟屏保，触摸即解锁

### 交互
- ✅ **电容触摸** — 顶栏下拉、底部上滑返回、手势判定（`lvgl_port_ui.c`）
- ✅ **手机遥控** — 播放/暂停/上下曲/音量/Seek，与设备端双向同步
- ✅ **配网页面** — SoftAP 门户（`192.168.4.1`），扫码/列表选网

## 工作原理

```
┌──────────────────────────────────────────────────────────────┐
│                      手机 (小米妙播 / 网易云)                  │
│   ┌────────────┐   ┌──────────────────┐   ┌──────────────┐  │
│   │ MiPlay /   │──▶│ 反控协议 / DLNA   │──▶│ HTTP 音频流   │  │
│   │ 选择投屏    │   │ SOAP + GENA      │   │ (URL)        │  │
│   └────────────┘   └────────┬─────────┘   └──────┬───────┘  │
└──────────────────────────────┼────────────────────┼──────────┘
                               │ 控制               │ HTTP 流
                               ▼                    ▼
┌──────────────────────────────────────────────────────────────┐
│                   ESP32-S3-Touch-LCD-1.85C                   │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  GMF Pipeline: io_http → aud_dec → aud_alc → io_i2s   │  │
│  │  (事件驱动：解码器上报采样率/位深 → I2S 原子重配置)      │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────┐  │
│  │  SSDP /  │ │ SOAP /   │ │ CST816S  │ │ ST77916 360x360│  │
│  │  mDNS    │ │ MiPlay   │ │ 触摸     │ │ 唱片+歌词+状态栏│  │
│  │  设备发现 │ │ 反控      │ │ 手势     │ │ (QSPI)         │  │
│  └──────────┘ └──────────┘ └──────────┘ └────────────────┘  │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ TCA9554      │  │ bat_monitor  │  │ wifi_provision   │   │
│  │ I/O 扩展     │  │ 电量/充电     │  │ SoftAP 配网门户   │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────────────┘
                               │
                               ▼ I2S (GPIO 38/48/47)
┌──────────────────────────────────────────────────────────────┐
│                     板载 I2S 功放 → 扬声器 / 耳机              │
└──────────────────────────────────────────────────────────────┘
```

## 快速开始

### 方式一：下载固件直接烧录

从 [Releases](https://github.com/MYHealer/esp-miply-1.85touch/releases) 下载 `v1.5.0` 的三件套。

```bash
pip install esptool

# 全量烧录（首次，COM 号按实际改。1.85touch 通常为 COM35）
esptool --chip esp32s3 -p COM35 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 dlna.bin
```

**仅升级固件**（保留已配置的 WiFi 与账号凭据）：

```bash
esptool --chip esp32s3 -p COM35 -b 460800 \
  write_flash 0x10000 dlna.bin
```

> ⚠️ 只烧 `0x10000` 一个地址即可。全量烧录会覆盖 `nvs` 分区，导致需要重新配网。

### 方式二：从源码构建

需要 **ESP-IDF v5.5+**（本项目在 v5.5.4 上验证；对 IDF 6.x 有兼容层）。

```powershell
# 必须用 PowerShell，先 dot-source 环境脚本
. E:\ESP\esp_env.ps1

# 编译
idf.py build

# 烧录
idf.py -p COM35 flash
```

> 本仓库已包含全部第三方组件源码，克隆后可直接编译，无需 `idf.py add-dependency`。

## 使用步骤

**① 烧录固件** → **② 上电** → **③ 配网** → **④ 投屏播放**

### 配网

1. 首次上电或未配置时，设备开启 SoftAP
2. 手机连接该热点，浏览器访问 `192.168.4.1`
3. 在配网页选择 WiFi 并输入密码（页面支持扫描附近网络）
4. 保存后设备自动重连并返回

### 投屏播放

**MiPlay（小米妙播）**：手机下拉控制中心 → 妙播 → 选择设备 `MS01B`
（mDNS 实例名；DLNA 设备名与主机名为 `ESP-1.85c`）

**DLNA**：手机打开网易云音乐 → 播放界面右上角投屏按钮 → 选择设备 `ESP-1.85c`

音乐从板载功放输出，屏幕显示唱片旋转 + 歌词 + 进度。

### 控制

| 操作 | 效果 |
|------|------|
| 触摸顶栏下拉 | 打开状态面板（音量/亮度/WiFi/电池） |
| 触摸底部上滑 | 返回上级界面 |
| 手机遥控方向键 | Seek 快进/快退 |
| 手机遥控 OK 键 | 播放/暂停 |
| 手机遥控关机键 | 进入/退出屏保 |
| 无操作 2 分钟 | 进入锁屏时钟（非播放态） |
| 触摸锁屏画面 | 立即解锁 |

## 技术架构

### GMF 音频管线

使用 **ESP-GMF**（Espressif 通用媒体框架），非旧版 ESP-ADF。

```
esp_gmf_pool → esp_gmf_pipeline
  ├── io_http      (HTTP 流拉取，PSRAM 缓冲)
  ├── aud_dec      (解码器：MP3/AAC/FLAC/WAV/OGG)
  ├── aud_alc      (ALC 软音量控制，0-100 → -64~0 dB)
  └── io_codec_dev (I2S 输出，直接 i2s_channel_write)
```

关键设计：
- **Job 消耗型**：每次 `_do_play()` 必须 `reset() + loading_jobs() + run()`，stop 后 working 链表被清空
- **事件驱动**：解码器解析流头后上报 `ESP_GMF_INFO_SOUND` 事件，I2S 原子重配置（disable → reconfig_clock → reconfig_slot → enable）
- **绕过 esp_codec_dev 软件音量**：其默认 volume=0（-96dB 静音），直接 `i2s_channel_write()` 输出

### 播放状态模型

四态枚举，是锁屏/刷新/UI 判定的唯一依据：

| 值 | 状态 | 含义 |
|---|---|---|
| 0 | `PS_STOPPED` | 停止 |
| 1 | `PS_PLAYING` | 播放 → **仅此状态阻止锁屏** |
| 2 | `PS_PAUSED` | 暂停 |
| 3 | `PS_TRANSITIONING` | 切歌过渡 |

来源有两路：设备本地 DLNA 管线状态（`get_state()`）与 MiPlay 协议上报镜像（`s_miplay_state`）。
协议层可能给出 `MIPLAY_PLAYER_STATE_UNKNOWN = -1`，**必须先校验有效区间再采纳**，否则 `-1` 会被
"非零即播放"的判定误认为播放中，导致锁屏永不触发。

### 双核分工

| Core | 任务 | 说明 |
|------|------|------|
| Core0 | WiFi + HTTP + DLNA/MiPlay 协议 + LVGL UI | 内核 + 5-6 |
| Core1 | GMF 音频管线（解码 + ALC + I2S） | 8-9（最高） |

### 内存布局（v1.5.0 全面对齐 PSRAM）

内部 SRAM 曾被任务栈占满（`int_free` 仅 ~94KB）而 PSRAM 闲置 6.1MB，导致投屏崩溃。
修复后内部第一段可用堆 **193KB → 257KB**，全部任务栈迁入 PSRAM。

| 配置项 | 值 | 说明 |
|---|---|---|
| `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | `y` | 允许任务栈放 PSRAM |
| `LV_USE_CLIB_MALLOC/STRING/SPRINTF` | `y` | LVGL 改用标准堆，受 `ALWAYSINTERNAL=512` 约束 |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | **`n`** | 实测置 `y` 会扩大 DMA 预留池（32K→96K），净亏 93KB |

> ⚠️ `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 的**语义与实测效果相反**，不要凭 Kconfig 描述推断。
> 改任何 SPIRAM 配置后必须回读启动日志的 `heap_init: At ... (N KiB)` 分段尺寸。

## 项目结构

```
esp_miplay-1.85c/
├── main/
│   ├── dlna.c                 # 主循环：DLNA + MiPlay 状态机、UI 刷新调度
│   └── CMakeLists.txt
├── components/
│   ├── custom_dlna/           # DLNA 设备实现（SSDP/SOAP/GENA）
│   ├── miplay/                # MiPlay 协议栈 + airkan 遥控
│   ├── gmf_core/ gmf_io/      # ESP-GMF 框架
│   ├── gmf_audio/ gmf_loader/ # GMF 音频元素
│   ├── lvgl_port/             # LVGL 移植 + UI（含 ui_ref/ 资源）
│   ├── tft_display/           # ST77916 QSPI 驱动
│   ├── echopal_board/         # 板级抽象 + TCA9554 扩展器
│   ├── i2s_pa/                # I2S 功放输出
│   ├── bat_monitor/           # 电量/充电检测
│   ├── chime/                 # 事件提示音
│   ├── wifi_provision/        # SoftAP 配网门户
│   ├── lyrics_fetch/          # 歌词抓取
│   ├── idf6_compat/           # IDF 6.x 兼容层（GPIO/UART/I2S/WiFi 重命名）
│   └── lvgl/ esp-adf-libs/ nghttp/ libwebp/ ...  # 第三方（已入库）
├── docs/                      # 开发文档与修复报告
├── partitions_dlna_example.csv
└── sdkconfig.defaults         # 配置改这里，不要改 sdkconfig（已忽略）
```

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| **v1.5.0** | 2026-09-18 | **PSRAM 内存全面对齐**（投屏崩溃根治）；配网扫描修复；2 分钟锁屏 + 播放状态判定修正；仓库独立可编译 |
| v1.4.0 | 2026-09-17 | MiPlay 重连卡顿修复；休眠屏保增强 |
| v1.3.8 | — | airkan 遥控左右键 seek + OK 键 toggle + 双管线音量统一 |
| v3.0–v3.4 | 2026-08 | ESP-ADF → ESP-GMF 迁移，采样率自适应，Seek/音量修复 |
| v2.x | 2026-08 | 唱片唱针动画、动态背景取色、歌词逐字高亮 |

## 已知事项

| 项目 | 状态 |
|---|---|
| `LV_DEF_REFR_PERIOD` 分叉 | 未处理 — `sdkconfig=33`（30fps）与 `sdkconfig.defaults=10`（100fps）不一致，提高帧率会增加 SPI 带宽与 CPU 占用，需另行评估 |
| 崩溃点 `lv_draw_sw_img.c:322` | 已不再复现。该路径走 transformed 慢路径，`int_largest` 仍为独立 32K 池天花板，高负载下若复现需针对碎片专项分析 |
| 播放中锁屏 | **设计如此** — 正在播放时不锁屏，避免遮盖封面动画。暂停/停止后 2 分钟锁屏 |

## 许可证

MIT
