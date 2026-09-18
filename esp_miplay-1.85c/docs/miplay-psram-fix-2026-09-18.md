# 1.85C MiPlay 投屏崩溃修复报告

**日期**：2026-09-18
**工程**：`E:\ESP\esp-miply-1.85touch\esp_miplay-1.85c`
**参考实现**：`参考项目/esp_miplay-main`、`参考项目/esp_miplay-Miaoban_fork`
**硬件**：ESP32-S3 + ST77916 1.85" QSPI 屏（引脚沿用本项目，未改动）

---

## 1. 问题现象

| 症状 | 描述 |
|---|---|
| 崩溃 | 投屏后约 5 秒设备重启 |
| 显示缺失 | 封面、歌名、进度、时长全部不显示 |
| 历史 | 用户反馈"以前能投，现在不行" |

崩溃现场固定指向 `lv_draw_sw_img.c:322`（RGB565A8 transformed 慢路径），3/3 次一致。

---

## 2. 根因

**内部 SRAM 被任务栈挤满，而 PSRAM 大量闲置。**

首次实测基线：

```
int_free    = 95,891 B      ← 内部 SRAM 仅剩 ~94 KB
int_largest = 31,744 B
psram_free  = 6,193,956 B   ← PSRAM 闲置 6.1 MB
psram_largest = 6,160,384 B
```

启动日志显示内部 SRAM 被切成三段**互不连续**的物理段：

```
heap_init: At 3FCB9298 len 00030478 (193 KiB): RAM
heap_init: At 3FCE9710 len 00005724 ( 21 KiB): RAM
heap_init: At 3FCF0000 len 00008000 ( 32 KiB): DRAM
esp_psram: Reserving pool of 32K of internal memory for DMA/internal allocations
```

封面旋转会让 LVGL 走 transformed 慢路径，需要额外分配图层缓冲，撞上内部内存上限 → 崩溃。同时该路径的失败也导致封面/元数据渲染中断。

---

## 3. 修复内容

### 3.1 消除 `sdkconfig` 与 `sdkconfig.defaults` 的分叉

**问题**：同一配置项在两个文件里取值相反，`.defaults` 的意图完全没有生效。

| 文件 | 原值 |
|---|---|
| `sdkconfig.defaults:21` | `# CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is not set` |
| `sdkconfig:3213` | `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` |

**修复**：`.defaults` 改为 `=y`，与 `sdkconfig` 一致。

### 3.2 启用 LVGL 标准堆分配

**问题**：LVGL 走内建分配器，无论实际需要多少都从内部 SRAM 割走固定 64 KB 池，且不受 PSRAM 规则约束。

| 配置项 | 原值 | 新值 |
|---|---|---|
| `LV_USE_CLIB_MALLOC` | `not set` | **`=y`** |
| `LV_USE_CLIB_STRING` | `not set` | **`=y`** |
| `LV_USE_CLIB_SPRINTF` | `not set` | **`=y`** |

**效果**：LVGL 每次分配都经过标准堆，受 `SPIRAM_MALLOC_ALWAYSINTERNAL=512` 约束 —— 超过 512 B 的自动去 PSRAM。副作用是 `LV_MEM_SIZE_KILOBYTES` 等键从 `sdkconfig` 中消失（不再需要固定池），这是预期结果。

### 3.3 纠正 `album_art` 任务栈虚开

| 项目 | 我们（原） | `esp_miplay-main:154` | 倍数 |
|---|---|---|---|
| `ALBUM_ART_TASK_STACK_BYTES` | 256 KB | **80 KB** | **3.2×** |

**修复**：`main/dlna.c:186` 改为 `(80U * 1024U)`。

**判定依据**：同一封面解码路径（lodepng / zlib inflate）在参考工程 80 KB 栈下可正常运行，说明 256 KB 无依据。虚开的代价是任务创建时 memset 整块、占用 PSRAM cache 映射，并掩盖真实栈需求。

**核对结果**：其余全部栈宏（`dlna.c` 另 2 个、`miplay.c` 7 个）与两个参考工程**逐字节一致**，无需调整。

### 3.4 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` —— 实测否决

按"全面对齐 Miaoban fork"曾一度设为 `=y`，实测后改回 `=n`。详见第 4 节。

**最终值**：`# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set`

---

## 4. 关键实测：`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 的反直觉效果

**该配置的语义与实测效果相反。** 三组对照，每组的启动日志实测值：

| 配置 | 第一段内部 RAM | 第二段 | 第三段 | DMA 预留池 | PSRAM 池 |
|---|---|---|---|---|---|
| 原始（未改动） | 193 KiB | 21 KiB | 32 KiB | 32 K | 6963 K |
| `TRY_ALLOCATE_WIFI_LWIP=y` | **164 KiB** ⚠️ | 21 KiB | 32 KiB | **96 K** ⚠️ | 7277 K |
| **`=n`（最终采用）** | **257 KiB** ✅ | 21 KiB | 32 KiB | 32 K | 6963 K |

**机理**：`=y` 时 IDF 会为 WiFi/LwIP 建立**更大的 DMA 预留池**（`esp_psram/app_startup.c:176` 调用 `esp_psram_extram_reserve_dma_pool`），因为 DMA 可达的 WiFi 缓冲仍需内部连续内存。预留池 32 K → 96 K，直接从第一段可用堆中扣除，**净亏 93 KiB**。

`=n` 反而让第一段达到 **257 KiB**，比原始 193 KiB 多出 64 KiB，优于两种改动前状态。

> IDF 自身的 `Kconfig.spiram.common` 中该项 `default n`，与本结论一致。

---

## 5. 验证结果

### 5.1 功能验证（用户实机确认）

| 项目 | 结果 |
|---|---|
| 投屏崩溃 | **已修复** — 连续播放不重启 |
| 封面图 + 旋转动画 | 正常 |
| 歌名 | 正常 |
| 进度 / 时长 | 正常 |
| 播放状态图标 | 正常 |

### 5.2 日志佐证

投屏期间出现此前从未出现的任务，证明媒体通路打通：

```
miplay_rtsp    49,152 B   PSRAM
media_rx      131,072 B   PSRAM    ← 媒体流接收
img_drain      12,288 B   PSRAM
lyric_fetch    49,152 B
```

渲染通路确认工作：

```
[DBG] set_cover: scaled 148x148 -> 148x148 px_size=43808 core=1
[DBG] cover rotate frame=1   angle=0
[DBG] cover rotate frame=129 angle=1754
[DBG] cover rotate frame=257 angle=3459
```

### 5.3 稳定性

```
Guru / panic / abort / Backtrace / Task WDT : 全部 0
```

投屏期间 `int_free` 在 88,787 ~ 96,891 之间健康波动，PSRAM 占用约 620 KB。

### 5.4 任务栈全部迁入 PSRAM

由 `dlna_create_task()` / `miplay_create_task()`（PSRAM 优先 + 内部回退）创建，日志确认全部走 PSRAM 路径，无一条 `PSRAM stack creation failed`：

```
album_art 262144→81920 | ssdp 16384 | ssdp_alive 12288 | gena_notify 32768
pos_notify 16384 | miplay_tcp 16384 | miplay_scan 12288 | miplay_lan 12288
airkan_probe 5120 | mdns_ann 12288 | ui_update 24576 | miplay_cli 262144
miplay_rtsp 49152 | img_drain 12288 | media_rx 131072 | lyric_fetch 49152
```

---

## 6. 判据与陷阱（供后续排查复用）

### 6.1 `int_largest` 恒为 32 K 不代表内存枯竭

`int_largest` 始终是 31,744 B（= 32 K − 1 K 池头），看似堵死的天花板，实际那是 `SPIRAM_MALLOC_RESERVE_INTERNAL` **单独锁死的一个独立 32 KiB DRAM 段**，与第一段大堆（257 KiB）是**两个分离的 heap**。只看 `int_largest` 会误判。

**正确做法**：读启动日志的 `heap_init: At ... (N KiB)` 分段尺寸。

### 6.2 抓取窗口截断会伪装成元数据故障

用固定短窗口抓投屏日志，会稳定截断在"连接已建立、播放尚未开始"的中间态，表现为：

```
source=miplay  但  pos=0 dur=0 state=0     ← 抓取假象，非固件问题
```

**正确判据**：看 `set_cover` / `cover rotate` 是否出现（真实渲染证据），而非 `pos/dur` 的瞬时值。

### 6.3 `sdkconfig` 永远赢过 `sdkconfig.defaults`

`sdkconfig.defaults` 只在该键**在 `sdkconfig` 中从未出现过**时提供初值。一旦 `sdkconfig` 写入具体值（含 `is not set` 这种否定形式），改 `.defaults` 对已有构建目录**完全无效**。

`idf.py reconfigure` **不会**用 `.defaults` 的新值覆盖已有 `sdkconfig`，它只补全新键。

**正确做法**：改配置后直接 `grep` `sdkconfig` 本身，不只看 `.defaults`。

### 6.4 串口独占导致抓取静默失败

第二个进程打开已被占用的 COM 口时不报错、不崩溃，只是读到极少字节 —— 与"设备无输出"无法区分。排查过程中曾因此浪费两轮抓取（180 s 仅得 2.6 KB、90 s 仅得 762 B）。

**正确做法**：抓取前先确认无残留进程占用串口。

---

## 7. 保留的调试探针

按要求保留，便于后续排障：

| 位置 | 输出 | 频率 |
|---|---|---|
| `main/dlna.c:2577` | `[DBG] mem: int_free/int_largest/psram_free/psram_largest` | 每 62.5 s（空闲 500 ms/tick × 125） |
| `main/dlna.c:2900` | `[DBG] miplay_connected_cb: connected/was/core` | 连接状态变化时 |
| `components/lvgl_port/lvgl_port_ui.c:2740` | `[DBG] cover rotate frame/angle/core` | 每 128 帧 |
| `components/lvgl_port/lvgl_port_ui.c:3275` | `[DBG] set_cover: scaled/px_size/core` | 每次设置封面 |

> 注意：`[DBG] mem` 的触发条件是 `tick % 125`，而空闲态 tick 周期为 500 ms，故实际间隔为 **62.5 秒**，非注释所写的 5 秒。

---

## 8. 未决事项

| 项目 | 状态 |
|---|---|
| `LV_DEF_REFR_PERIOD` 分叉 | **暂不处理** — `sdkconfig=33`（30 fps）、`sdkconfig.defaults:189=10`（100 fps）。非内存项，100 fps 会增加 SPI 带宽与 CPU 占用，需另行评估 |
| `custom_dlna` 等组件比对 | 未做 — 本次仅对齐内存侧 |
| 原始崩溃 `lv_draw_sw_img.c:322` | 未再复现。`int_largest` 天花板未变（仍是独立 32 K 池），若后续在高负载场景复现，需针对该池的碎片做专项分析 |

---

## 9. 改动文件清单

| 文件 | 改动 |
|---|---|
| `sdkconfig` | `LV_USE_CLIB_MALLOC/STRING/SPRINTF` → `=y`；`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 保持 `not set` |
| `sdkconfig.defaults` | `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` → `=y` |
| `main/dlna.c:186` | `ALBUM_ART_TASK_STACK_BYTES` 256 KB → 80 KB |
