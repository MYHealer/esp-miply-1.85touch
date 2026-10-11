# ESP32-S3-Touch-LCD-1.85C — 基于小米妙播协议的原生小米音箱

> 在 **ESP32-S3-Touch-LCD-1.85C 一体化开发板**上实现的原生小米音箱：协议级对接
> **小米妙播（MiPlay）**，可被手机控制中心直接发现与投屏，支持反向控制。
> 同时兼容 DLNA 投屏。1.85 寸 360×360 圆形 QSPI 屏显示唱片动画与歌词，电容触摸 + 手机遥控双向控制。

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
- ✅ **MiPlay（小米妙播）** — 协议级对接：mDNS 双服务发现 + TCP 8899 加密帧 + 反向控制（[详见核心协议](#小米妙播miplay)）
- ✅ **airkan 遥控** — 小米妙享遥控器方向键 Seek、OK 键播放暂停、关机键切回屏保（[详见核心协议](#小米-airkan-遥控)）
- ✅ **DLNA** — 网易云/QQ音乐/酷狗等 DLNA App 投屏
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

## 核心协议

本项目最核心的部分不是音频播放（那是 ESP-GMF 的功劳），而是**让手机把它当作一台原生小米音箱**。
这由两个小米私有协议共同实现：**小米妙播（MiPlay）**负责发现与推流，**airkan 遥控**负责遥控器按键。

### 小米妙播（MiPlay）

MiPlay 是小米的局域网投屏协议，手机下拉控制中心的「妙播」列表就是靠它发现设备。

**① 服务发现（mDNS）**

注册两个服务，缺一不可：

| 服务 | 类型 | 作用 |
|------|------|------|
| `_mi-connect._udp` | TXT 广播 | 让手机知道「这里有一台可投屏设备」 |
| `_lyra-mdns._udp` | TXT 广播 | 携带 Lyra 能力位与 AppData，决定设备出现在哪个 picker |

TXT 里有两个**不能自作聪明改**的字段：

- `appsdata` 固定为 base64 `gQAEBIMiww==`（解码 `81 00 04 04 83 22 C3`，其中 `0x0483` 是 Lyra 能力位入口、`0x22C3`=8899 是控制端口）。
  用 PC 端自算的描述符会导致 HyperOS 置 `hasLyraBymiplay=true`，**反而把接收端拒绝加入音频 picker**。
- `AppData` 二进制里设备类型字节是 `0x03`（TV）而不是 PC 的 `0x15`。
  实测只有以 TV 身份出现，手机的遥控面板才会打开（见下文 airkan）。

**② 控制通道（TCP 8899）**

手机连上 8899 后，所有控制走**加密二进制帧**：

```
[magic 0x24][... header ...][cmd:2B][seq:2B][payload]
      └─ payload 用 SafetyData v1 封装：AES-128-CBC + CRC32(MPEG2) 完整性校验
```

- **帧魔数 `0x24`（`'$'`）**：解析时先按它同步。陷阱是歌词/封面/密文里也大量出现 `'$'`，
  所以不能只看首字节，还要校验后续头部是否像 ASCII 文本才能确认是控制帧。
- **加密**：`SafetyData v1`，固定 `AES_BLOCK_LEN 16`，CRC32 多项式 `0x04C11DB7`。
  密钥来自手机下发的 safety challenge 交换，没有它连不上媒体通道。
- **命令表**（节选，完整 22 条已全部实现）：

| 方向 | 命令 | 值 | 说明 |
|------|------|-----|------|
| 手机→设备 | `CMD_OPEN_DEVICE` | `0x0000` | 打开设备会话 |
| 手机→设备 | `CMD_PAUSE` / `CMD_RESUME` | `0x0004` / `0x0006` | 暂停 / 继续 |
| 手机→设备 | `CMD_SET_VOLUME` / `CMD_GET_VOLUME` | `0x000C` / `0x000E` | 音量 |
| 手机→设备 | `CMD_GET_MEDIA_INFO` | `0x0014` | 拉取媒体信息（封面/标题） |
| 手机→设备 | `CMD_SET_PLAY_SOURCE` | `0x0040` | 注册播放源（推流前必做） |
| 手机→设备 | `CMD_HEARTBEAT` | `0x001A` | 心跳保活 |
| 手机→设备 | `CMD_SAFETY_*` | `0x0028`/`0x1400` | 密钥协商与鉴权 |
| 设备→手机 | `CMD_NOTIFY` | `0x0022` | **反向控制**（见下） |

**③ 反向控制（设备的遥控器按钮 → 手机）**

这是「原生音箱」体验的关键：设备端按暂停，手机也要跟着暂停。统一用 `CMD_NOTIFY (0x0022)`
发二进制 TLV：

```
[key_len:1B][key_bytes][value_type:1B][value_data]
```

| 动作 | key | value_type | 说明 |
|------|-----|-----------|------|
| 暂停/继续 | `key-pause` / `key-resume` | `0x00` (bool) | 值恒为 `0x01` |
| 上/下一曲 | `key-prev` / `key-next` | `0x00` (bool) | |
| Seek | `key-seek` | `0x09` | 8 字节大端毫秒 |
| 音量同步 | `volume`（**无 `key-` 前缀**） | `0x07` | 4 字节大端百分比 |

⚠️ 两个坑：
- `0x0004`/`0x0006` 只用于**手机→设备**方向，设备回控必须用 `0x0022` NOTIFY，方向写反手机无响应。
- 音量回控的键名是裸的 `volume`，不是 `key-volume`——与 `SetVolume` 之后的 sender-volume 通知字节格式保持一致。
- HyperOS 收到 `key-pause`/`key-resume` 后**不保证回显状态**，所以设备端在发包成功后要立即本地投影新状态，
  否则下一次 toggle 和 `GET_STATE` 会读到陈旧状态（表现为按两下才生效）。

### 小米 airkan 遥控

airkan（杜坎 `airkan_sdk`）是小米设备互联的**遥控器按键通道**。手机把设备识别为 TV 后，
遥控面板的方向键/OK 键/电源键就是走这条通道，**不走 MiPlay 的 8899**。

**① 两个端口，角色不同（别混）**

| 端口 | 协议 | 作用 |
|------|------|------|
| **6095** | HTTP | 认证入口（`getInfo` / `requestAuth` / `completeAuth` / `cancelAuth`） |
| **6091** | TCP | 遥控按键长连接（真正收按键帧的地方） |

6091 这个端口是被反复猜错后才锁定的。证据链（HyperAll 反编译）：

```
AirkanService.java:1010  P(new f(this, h.IP, str), 2)
  → :1272  构造函数 f(...) { this(hVar, 6091, ...); }
  → :420   this.A.f(fVar.f6142d, fVar.f6141c)   (ip, port)
  → remote/i.java:76   f(String ip, int port) → RCClientThreadV2.connect
  → RCClientThreadV2.java:341  socketChannel.connect(new InetSocketAddress(mIp, mPort))
```

> 早期在 6094–6100 里试探，**恰好全部避开了 6091**，所以按键永远收不到。保留 6090/6092 作兜底。

**② 版本号门槛**

认证时上报的版本必须 **`> 0x01000000`**，本设备报 `0x01000008`。手机侧 `onVersionResponse`
用它判断「设备是否支持遥控」：报 `1` 时手机只会反复 `getInfo`（每次按键重探），永不推进——低版本被当成「不支持」。

**③ 按键帧结构**

```
[type:1B][len:2B BE]                        ← RCHeader
[code:1B][id:4B BE][data_len:2B BE]         ← SendKeyHeader
[subcode:1B][value:4B or 8B]...             ← TLV
```

TLV 里 `subcode 2 = action`（`0=down`, `1=up`）、`subcode 3 = keyCode`。本设备认的 keyCode：

| keyCode | 按键 | 动作 |
|---------|------|------|
| 19 / 20 | next / prev | 上/下一曲 |
| 21 / 22 | LEFT / RIGHT | Seek ∓15s |
| 23 / 66 | DPAD_CENTER / ENTER | 播放暂停 toggle |
| 24 / 25 | VOL+ / VOL- | 音量 ±5 |
| 26 | POWER | 进入/退出屏保 |

**④ 两个实现要点**

- **静默失败**：airkan 连接失败时不重试、不报错，所以只能靠日志判断走到了哪一步。一旦没连上，现象是「遥控器和手机的连接显示正常，但按键毫无反应」。
- **OK 键消抖**：手机按一次 OK 会连发多帧 `action=0`，本设备做 300ms 窗口去重，否则一按跳两次。

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

## 参考项目及致谢

本项目建立在多个上游开源工作之上，特此致谢。

### 参考项目

| 项目 | 说明 |
|------|------|
| [MYHealer/dlna-esp32](https://github.com/MYHealer/dlna-esp32) | 本项目的 DLNA 前身：纯 DLNA 投屏实现，是本仓库协议层与音频管线的起点 |
| [MYHealer/airkan-sdk](https://github.com/MYHealer/airkan-sdk) | airkan 遥控 SDK：小米妙享遥控器按键通道的独立实现，本仓库 airkan 部分的对标与参考 |
| [rosienosiesie/FusionPlay-Android](https://github.com/rosienosiesie/FusionPlay-Android) | Android 端 MiPlay 播放器，本仓库 MiPlay 协议实现的关键参考（音量回控 `volume` 字节格式等即据此对齐） |

### 第三方组件致谢

| 组件 | 作者 / 组织 | 用途 |
|------|------------|------|
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif Systems | 芯片 SDK 与构建系统 |
| [ESP-GMF](https://github.com/espressif/esp-gmf) | Espressif Systems | 音频管线框架（`io_http → aud_dec → aud_alc → io_i2s`） |
| [LVGL](https://github.com/lvgl/lvgl) | LVGL 社区 | 圆屏 UI 渲染（唱片动画 / 歌词 / 状态栏） |
| [esp-adf-libs](https://github.com/espressif/esp-adf-libs) | Espressif Systems | 音频编解码库（MP3/AAC/WAV/FLAC/OGG） |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | Google | 封面 WebP 解码 |
| [nghttp](https://github.com/nghttp2/nghttp2) | Tatsuhiro Tsujikawa | HTTP/2 客户端（歌词抓取） |

### 协议实现致谢

- **小米妙播（MiPlay）** 与 **airkan 遥控** 的协议细节来自对官方客户端与固件的逆向分析。
  感谢 [HyperAll](https://github.com/) 等社区项目的反编译工作为 airkan 端口定位提供了关键证据链。
- 感谢 ESP-GMF 团队在音频管线上游的持续维护，本项目大量依赖其解码与重采样能力。

## 免责声明

本项目为独立的逆向工程实践，**仅供学习与个人研究使用**，与小米公司无任何隶属、认可或赞助关系。

- ⚠️ **仅供学习使用** — 本项目用于协议分析、嵌入式开发与音视频技术的学习交流，请勿用于任何商业用途。
- 🚫 **禁止售卖** — 不得将本项目（含源码、固件、衍生作品）用于销售、批量生产或任何形式的商业牟利。
- 📄 **责任自负** — 使用本项目即表示你自行承担全部责任，包括对小米服务条款的合规性。作者不提供任何担保，亦不承担任何责任。
- ™️ **商标归属** — 「小米」「妙播」「妙享」「米家」及相关标识均为小米公司商标，本项目对其的使用仅为技术说明性引用。

## 许可证

MIT
