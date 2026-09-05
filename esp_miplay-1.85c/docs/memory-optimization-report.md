# ESP32-S3 内存优化审查报告

> 项目：ESP32-S3-Touch-LCD-1.85C（DLNA + MiPlay 双协议）
> 审查日期：2026-09-05
> 范围：`main/dlna.c`、`components/miplay/miplay.c`、`components/custom_dlna/custom_dlna.c`
> 方式：Espressif 官方文档检索 + `build/dlna.map` 符号实量 + 源码静态声明核对
> 状态：**只调研不改代码**

---

## 一、芯片 & 项目内存背景

| 资源 | 容量 | 说明 |
|------|------|------|
| 内部 SRAM (DRAM) | ~370KB | Stack、heap、RTOS、DMA 缓冲（速度最快，稀缺）|
| PSRAM | 8MB | 大缓冲、音频/封面/管线数据块 |
| Flash | 16MB | 代码 + 只读数据 + 分区 |

**核心原则**：只有 DMA 缓冲、中断路径、关键实时路径才必须留内部 SRAM；大块数据一律进 PSRAM。

---

## 二、三路 PSRAM 优化手段（官方机制）

| 手段 | 语法 | 适用 | Kconfig 开关 |
|------|------|------|--------------|
| **静态 bss 搬移** | `EXT_RAM_BSS_ATTR` | 未初始化的全局/静态数组 | `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`(已开) |
| **运行时堆** | `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` | 显式指定分配位置 | `CONFIG_SPIRAM` |
| **malloc 自动路由** | 标准 `malloc()` | 超阈值大块自动走 PSRAM | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=<阈值>` |

参考：docs.espressif.com `/projects/esp-techpedia/.../reduce-ram-usage.html#chip-with-psram`

---

## 三、现状评估：主体优化已到位 ✅

### 3.1 map 文件实量结果（`build/dlna.map`）

**`.ext_ram.bss` 段已占用 0xcc778 ≈ 837KB PSRAM**，三个源文件的静态大块已全部迁出：

| 符号 | 大小 | 所在组件 |
|------|------|----------|
| `s_miplay_media_queue_storage` (队列) | 0xc000 ×3 | main (dlna.c) |
| `s_miplay_cover_url` 等媒体字段 | 0x61140 | main (dlna.c) |
| 封面 / 元数据 8 个字段 | 0xc000 ~ | main (dlna.c) |
| `s_metadata[16384]` 等 | 0x4000 | custom_dlna |
| 加密/信封缓冲 | 0x10019 | miplay |

**三个文件内部 bss 残留仅剩：**
- `s_sessions[3]` → 0x1b0**（432B，可再搬）**
- `s_mac` → 0x6 / 各类指针、句柄 → 0x4

**结论：静态/显式动态的大块已基本清空。**

### 3.2 运行时分配核对

- **Task 栈**：`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` — TCB 最小化，栈在 PSRAM ✅
  - MiPlay 各任务栈：TCP 16K、CLIENT 256K、RTSP 48K、MEDIA 128K（全部 PSRAM）
- **动态 malloc**：miplay 12 处 + custom_dlna 8 处 + dlna 7 处 strdup 已全改 `heap_caps_malloc(SPIRAM)` ✅

### 3.3 Kconfig 现状

```
CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512      # >512B 自动走 PSRAM
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536  # 保命保留区
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP         # 未开（见 4.2，已决策不开）
CONFIG_SPIRAM_BOOT_INIT=y                    # GMF 分配器据此自动走 PSRAM
```

### 3.4 全项目内部 bss 总览（map 文件实测）

对 `build/dlna.map` 中地址在 `0x3f*` 的 `.bss` 符号做全量聚合：

| 指标 | 数值 |
|------|------|
| 内部 `.bss` 总计 | **9185 B（9.0 KB）** |
| 符号 ≥256B | 6 个（4580 B）|
| 符号 <256B | 560 个 |
| **本项目组件中 ≥256B 的符号** | **0 个** |

内部 bss 剩余大符号（全部第三方，全部不可搬）：

| 符号 | 大小 | 不可搬原因 |
|------|------|-----------|
| `s_coredump_stack` | 2148 | coredump 现场（见 5.1 官方原文）|
| `s_reg_dump` | 588 | coredump 现场 |
| `kGammaToLinearTab` | 512 | `const` 查表，非 bss，`EXT_RAM_BSS_ATTR` 不适用 |
| `pxReadyTasksLists` | 500 | FreeRTOS 调度器核心，高频访问 |
| `lv_global` | 492 | LVGL 全局状态，渲染高频访问 |
| `mdns_task_buffer` | 340 | mDNS 任务相关 |

---

## 四、还能继续腾挪的候选（按收益排序）

| # | 位置 | 大小 | 结论 |
|---|------|------|------|
| 1 | `miplay.c:277 s_sessions[3]` | 432B | ✅ **已完成**：加 `EXT_RAM_BSS_ATTR`，map 对账 `.ext_ram.bss` 精确 +432B |
| 2 | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | ~30KB | ⛔ **决策：不开**（见 4.2）|

### 4.2 决策：不开 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`

**这个开关只影响堆分配，不影响带宽/速率。** IDF 源码 `esp_wifi/esp32s3/esp_adapter.c`：

```c
IRAM_ATTR void *wifi_malloc(size_t size) {
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT|MALLOC_CAP_SPIRAM,
                                          MALLOC_CAP_DEFAULT|MALLOC_CAP_INTERNAL);
#else
    return malloc(size);
#endif
}
```

同一文件 `wifi_create_queue()` 却**硬编码 `MALLOC_CAP_INTERNAL`** —— Wi-Fi 控制队列与 DMA 结构不受此开关影响，IDF 已守住关键路径。

**不开的三条理由：**

1. **收益近零**：本项目内部 SRAM 未告急（DLNA/MiPlay 双协议 + 双 GMF 管线均正常）。
2. **风险实在**：Wi-Fi 收包在 ISR 上下文，把其数据缓冲搬 PSRAM 会与 flash 写争抢（见 5.2 官方原文）。
3. **延迟不是主因**：PSRAM 访问多几十 ns，相对网络毫秒级是噪声；真正的风险是 cache 禁用期的 illegal access。

`heap_caps_malloc_prefer` 是"尽力而为"语义（PSRAM 失败自动回退内部），故开了也不会因 PSRAM 耗尽崩溃 —— 风险可控，但**没必要**。

### 4.3 已排除项：GMF 解码缓冲（**已确认无需处理**）

原先列为"需实测"，经源码核对已有确定结论：

`components/gmf_core/oal/esp_gmf_oal_mem.c` 中，当 `CONFIG_SPIRAM_BOOT_INIT=y`（本项目已开）时：

```c
esp_gmf_oal_calloc()  → heap_caps_malloc(..., MALLOC_CAP_SPIRAM)   // PSRAM
esp_gmf_oal_realloc() → heap_caps_realloc(..., MALLOC_CAP_SPIRAM)  // PSRAM
esp_gmf_oal_strdup()  → heap_caps_malloc(..., MALLOC_CAP_SPIRAM)   // PSRAM
```

GMF 管线的 `aud_dec`/`aud_alc`/payload buffer 全部由上述分配器创建，**已自动落在 PSRAM，无需改动**。仅 `esp_gmf_oal_calloc_inner` 会优先内部 RAM，但那专供 DMA 路径使用，属正确设计。

---

## 五、不建议动的（必须留内部 SRAM）

### 5.1 coredump 相关变量 —— 官方原文

> "Apart from the crashed task's TCB and stack, data located in the external RAM will **not be stored in the core dump file**, this include variables defined with `EXT_RAM_BSS_ATTR` or `EXT_RAM_NOINIT_ATTR` attributes, as well as any data stored in the `extram_bss` section."
> — ESP-IDF 编程指南 / Core Dump / Core Dump Memory Regions

**结论**：`s_coredump_stack`(2148B)、`s_reg_dump`(588B) 若搬进 PSRAM，**崩溃时不会被记录**，等于自废 coredump 调试能力。必须留内部 SRAM。

### 5.2 flash 写期间 PSRAM 不可访问 —— 官方原文

> "When flash cache is disabled (for example, if the flash is being written to), the external RAM also becomes inaccessible. Any read operations from or write operations to it will lead to an **illegal cache access exception**."
> — ESP-IDF 编程指南 / Support for External RAM / Restrictions

**这是反对开 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 的最硬依据**：Wi-Fi 收包在 ISR 上下文，与 NVS/OTA 的 flash 写共用 SPI 总线与 cache。

### 5.3 本项目已正确规避该陷阱的代码（勿改）

`components/miplay/miplay.c:225` `miplay_save_volume()`：

```c
/* 内部 SRAM 栈（不指定 WithCaps），2KB 足够 NVS 写 */
xTaskCreate(miplay_nvs_save_task, "nvs_vol", 2048, a, 3, NULL);
```

NVS 写属 flash 操作，会临时禁用 cache，若从 PSRAM 栈任务调用会触发 illegal cache access。此处刻意用 `xTaskCreate`（内部 SRAM 栈）而非 `xTaskCreateWithCaps`，**符合官方约束，不可改为 PSRAM 栈**。

### 5.4 其他不可动项

- I2S / PCM5102A DMA 缓冲（`MALLOC_CAP_DMA` 强制内部 SRAM）
- `pxReadyTasksLists`（FreeRTOS 调度器，每 tick 高频访问）
- `lv_global`（LVGL 全局状态，渲染循环高频访问）
- `s_rtsp_buf`（仅指针，内容已 PSRAM，别重复标注）
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=64KB` 保留区 — **保命垫，别砍**

---

## 六、结论（静态内存优化已完结）

- **静态大块、动态分配、Task 栈、GMF 管线缓冲** 全部已落 PSRAM。
- 全项目内部 `.bss` 仅剩 **9.0 KB**，其中**本项目组件 ≥256B 符号为 0 个**。
- 剩余 6 个大符号全部第三方，且**均有官方依据必须留内部**（coredump 不记录 ext_ram、RTOS 高频、LVGL 高频）。
- `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` **决策不开**：收益近零 + ISR 与 flash 写竞争风险。

> **静态内存搬运已到终点。** 后续若需进一步优化，方向应为运行时堆调优（如 ringbuf 32KB 容量），而非继续搬运；且当前 DLNA/MiPlay 双协议功能均正常，无迫切必要。

## 七、本报告后续修订

| 日期 | 修订内容 |
|------|----------|
| 2026-09-05 | 补充 4.3：核对 `esp_gmf_oal_mem.c`，确认 GMF 解码缓冲在 `CONFIG_SPIRAM_BOOT_INIT=y` 下已自动走 PSRAM，原"需实测"结论作废 |
| 2026-09-05 | 补充 3.4：map 文件全量聚合，内部 bss 仅 9.0KB，本项目组件 ≥256B 符号为 0 |
| 2026-09-05 | 补充 4.2：`wifi_create_queue` 硬编码 `MALLOC_CAP_INTERNAL`，WIFI_LWIP 开关不影响 DMA/队列；决策不开 |
| 2026-09-05 | 补充第五章：引入 ESP-IDF 官方原文（coredump 不记录 ext_ram、flash 写时 PSRAM 不可访问），并标注 `miplay_save_volume()` 这一已正确规避陷阱的代码 |

---

## 附：关键参考

- ESP-IDF external-ram 文档：`/projects/esp-idf/.../api-guides/external-ram.html`
- ESP 内存优化：`/projects/esp-techpedia/.../reduce-ram-usage.html`
- GMF 内存打印：`ESP_GMF_MEM_SHOW(TAG)` 或 `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`