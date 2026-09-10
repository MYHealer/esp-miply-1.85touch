/*
 * 网易云歌词获取 — ESP32 直接调网易云音乐 API
 *
 * 流程：歌名+歌手 → 网易云优先，失败后 fallback QQ/酷我 → 解析 LRC
 * 使用 esp_http_client + jsmn JSON 解析
 */

#include "lyrics_fetch.h"
#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "jsmn.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "LYRIC";

/* ── 统一的"可唱字符"判定 ──
 * 三处调用方（伪klyric生成 / 总字数统计 / 逐字映射）必须用同一套标准，
 * 否则同一个字符一边计入 word entry、一边被跳过，高亮就会整体偏移。
 * 不可唱 = ASCII标点空白 + 全角ASCII区(U+FF01..FF5E) + CJK标点区(U+3000..U+300F)。
 * 返回该字符的 UTF-8 字节数。 */
static int lyric_char_bytes(unsigned char c, bool *is_sung)
{
    int bytes = 1;
    bool sung = true;
    if (c >= 0xE0) {
        bytes = 3;
    } else if (c >= 0xC0) {
        bytes = 2;
    } else {
        /* ASCII 标点/空白不可唱 */
        if (c == ' ' || c == ',' || c == '.' || c == '!' || c == '?' ||
            c == ';' || c == ':' || c == '-' || c == '\'' || c == '"') {
            sung = false;
        }
    }
    if (is_sung) *is_sung = sung;
    return bytes;
}

/* 结合后续字节判断全角/CJK 标点（需传入当前字符指针） */
static bool lyric_is_sung_char(const char *p, int *bytes_out)
{
    unsigned char c = (unsigned char)*p;
    bool sung = true;
    int bytes = lyric_char_bytes(c, &sung);

    if (bytes == 3 && sung) {
        /* 全角 ASCII 区 U+FF01..FF5E（ＥＦ BC 81..BF）→ 不可唱 */
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC) {
            unsigned char c2 = (unsigned char)p[2];
            if (c2 >= 0x81 && c2 <= 0xBF) sung = false;
        }
        /* CJK 标点区 U+3000..U+301F（E3 80 80..9F / E3 81 ..） */
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80) {
            unsigned char c2 = (unsigned char)p[2];
            if (c2 >= 0x80 && c2 <= 0x9F) sung = false;
        }
        /* 中文标点 ，。！？；：、… 落在 U+3000 段或全角段，已覆盖；
         * 补充 U+2026(…) / U+2014(—) 等常用省略号破折号 */
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80) {
            unsigned char c2 = (unsigned char)p[2];
            if (c2 == 0xA6 || c2 == 0x94) sung = false;   /* … — */
        }
    }
    if (bytes_out) *bytes_out = bytes;
    return sung;
}


/* ── 静态数据 ── */
static EXT_RAM_BSS_ATTR lyric_data_t     s_lyric_data;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_fetch_task;
static uint32_t          s_fetch_gen;

/* ── klyric 逐字时间戳（扁平数组，共 ~2.5KB）── */
/* 容量按"128 行 × 平均 12 字"估算。原来 256 字只够 ~25 行，
 * 一首 44 行的歌会在中途撑满，后半首完全没有逐字数据。
 * 三个数组都在 PSRAM（EXT_RAM_BSS_ATTR），扩到 1536 只多占 ~12KB。 */
#define KLYRIC_MAX_WORDS   1536
static EXT_RAM_BSS_ATTR int  s_klyric_start[KLYRIC_MAX_WORDS];   /* 每个字的绝对起始毫秒 */
static EXT_RAM_BSS_ATTR int  s_klyric_end[KLYRIC_MAX_WORDS];     /* 每个字的绝对结束毫秒 */
static int  s_klyric_count;                     /* 总字数 */
static EXT_RAM_BSS_ATTR int  s_klyric_line_first[LYRIC_MAX_LINES]; /* 每行在 s_klyric_start 中的起始索引 */
static EXT_RAM_BSS_ATTR int  s_klyric_line_time[LYRIC_MAX_LINES];  /* 每行的起始时间（ms），用于按时间匹配 */
static int  s_klyric_line_count;                 /* 有 klyric 数据的行数 */
static EXT_RAM_BSS_ATTR int  s_lrc_to_klyric[LYRIC_MAX_LINES];   /* LRC行 → klyric行 映射，-1=无 */

/* HTTP/TLS + JSON 解析路径局部对象较多，任务栈优先放 PSRAM。 */
#define LYRIC_TASK_STACK_SIZE  (48U * 1024U)

static void lyrics_delete_current_task(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    StackType_t *stack = NULL;
    StaticTask_t *tcb = NULL;

    if (xTaskGetStaticBuffers(current, &stack, &tcb) == pdTRUE) {
        vTaskDeleteWithCaps(current);
    } else {
        vTaskDelete(NULL);
    }
}

static BaseType_t lyrics_create_task(TaskFunction_t task, const char *name,
                                     uint32_t stack_bytes, void *arg,
                                     UBaseType_t priority, TaskHandle_t *handle,
                                     BaseType_t core)
{
    const uint32_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    if (handle) *handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(task, name, stack_bytes,
                                                     arg, priority, handle,
                                                     core, stack_caps);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "Task %s created with PSRAM stack (%u bytes)",
                 name, (unsigned)stack_bytes);
        return ret;
    }

    ESP_LOGW(TAG, "Task %s PSRAM stack creation failed; trying internal stack",
             name);
    return xTaskCreatePinnedToCore(task, name, stack_bytes, arg,
                                   priority, handle, core);
}

/* ── klyric 解析前向声明 ── */
static void parse_klyric(const char *klyric_text);

/* ── HTTP 响应缓冲 ── */
/* 初始缓冲；http_event_handler 会在不够时自动 realloc 扩容，
 * 所以这里给个常见值即可，不必按最大响应预留。
 * 历史 bug：cloudsearch 响应实测 12579 字节，而搜索缓冲只有 8192，
 * 数据被静默截断 → 无效 JSON → NetEase 全部失败 → 退到 Kuwo 二手歌词。 */
#define HTTP_SEARCH_BUF          16384
#define HTTP_FALLBACK_SEARCH_BUF 16384
#define HTTP_LYRIC_BUF           32768

typedef struct {
    char    *buf;
    int      len;
    int      cap;
} resp_ctx_t;

/* ── URL 编码（UTF-8 安全）── */
static int url_encode(const char *src, char *dst, int dst_size)
{
    const char *hex = "0123456789ABCDEF";
    int di = 0;
    for (int i = 0; src[i] && di < dst_size - 3; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ── HTTP 事件回调：累积响应体 ── */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        /* 缓冲不足时按需扩容（cloudsearch 响应实测 12.5KB，远超原 8KB 缓冲）。
         * 旧实现静默丢弃多余数据，导致截断的 JSON 解析失败却毫无提示。 */
        if (ctx->len + evt->data_len + 1 > ctx->cap) {
            int need = ctx->len + evt->data_len + 1;
            int new_cap = ctx->cap;
            while (new_cap < need) new_cap *= 2;
            char *nb = realloc(ctx->buf, new_cap);
            if (!nb) {
                ESP_LOGW(TAG, "resp buffer grow failed (need %d), truncating", need);
                break;
            }
            ctx->buf = nb;
            ctx->cap = new_cap;
        }
        memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
        ctx->len += evt->data_len;
        ctx->buf[ctx->len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── HTTP GET/POST，返回响应体（调用方 free）── */
static char *http_request_ex(const char *url, const char *post_data,
                             const char *content_type, int buf_size,
                             const char *referer)
{
    resp_ctx_t ctx = { .buf = malloc(buf_size), .len = 0, .cap = buf_size };
    if (!ctx.buf) return NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(ctx.buf); return NULL; }

    esp_http_client_set_header(client, "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120");
    if (referer && referer[0]) {
        esp_http_client_set_header(client, "Referer", referer);
    }

    esp_err_t err;
    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type",
            content_type ? content_type : "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(ctx.buf);
        return NULL;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status=%d, content_length=%d, received=%d",
             status, content_length, ctx.len);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        free(ctx.buf);
        return NULL;
    }

    if (content_length > 0 && ctx.len < content_length) {
        ESP_LOGW(TAG, "response TRUNCATED: got %d of %d bytes (url=%.64s)",
                 ctx.len, content_length, url);
    }

    if (ctx.len == 0) {
        free(ctx.buf);
        return NULL;
    }

    return ctx.buf;
}

static char *http_request(const char *url, const char *post_data,
                          const char *content_type, int buf_size)
{
    return http_request_ex(url, post_data, content_type, buf_size,
                           "https://music.163.com");
}

/* ── jsmn 辅助：在 token 数组中查找 key ── */
static int json_find_key(const char *json, jsmntok_t *tokens, int num_tokens,
                         int start, const char *key)
{
    int key_len = strlen(key);
    for (int i = start; i < num_tokens; i++) {
        if (tokens[i].type == JSMN_STRING) {
            int len = tokens[i].end - tokens[i].start;
            if (len == key_len && memcmp(json + tokens[i].start, key, len) == 0) {
                return i + 1;  /* 返回 value 的 token 索引 */
            }
        }
        /* 跳过子树 */
        if (tokens[i].type == JSMN_OBJECT || tokens[i].type == JSMN_ARRAY) {
            int skip = tokens[i].size;
            for (int j = i + 1; j < num_tokens && skip > 0; j++) {
                if (tokens[j].type == JSMN_OBJECT || tokens[j].type == JSMN_ARRAY) {
                    skip += tokens[j].size;
                }
                skip--;
            }
            i += skip;
        }
    }
    return -1;
}

/* forward declaration */
static int jsmn_container_end(const jsmntok_t *tokens, int num, int idx);

/* ── 在容器的直接子 key 中查找（不递归进入子容器） ── */
static int json_find_key_in_object(const char *json, jsmntok_t *tokens, int num_tokens,
                                   int container_idx, const char *key)
{
    if (container_idx < 0 || container_idx >= num_tokens) return -1;
    if (tokens[container_idx].type != JSMN_OBJECT) return -1;
    int key_len = strlen(key);
    int end = jsmn_container_end(tokens, num_tokens, container_idx);
    int i = container_idx + 1;
    while (i < end && i < num_tokens) {
        if (tokens[i].type == JSMN_STRING) {
            int len = tokens[i].end - tokens[i].start;
            if (len == key_len && memcmp(json + tokens[i].start, key, len) == 0) {
                return i + 1;  /* 返回 value 的 token 索引 */
            }
        }
        /* 跳过这个 key-value 对 */
        i++;  /* skip key */
        if (i < end && i < num_tokens) {
            if (tokens[i].type == JSMN_OBJECT || tokens[i].type == JSMN_ARRAY) {
                i = jsmn_container_end(tokens, num_tokens, i);
            } else {
                i++;  /* skip primitive value */
            }
        }
    }
    return -1;
}

/* ── jsmn 辅助：提取 token 的字符串值 ── */
static int json_get_string(const char *json, jsmntok_t *tok, char *out, int out_size)
{
    int len = tok->end - tok->start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, json + tok->start, len);
    out[len] = '\0';
    return len;
}

/* ── jsmn 辅助：提取 token 的整数值 ── */
static unsigned long json_get_int(const char *json, jsmntok_t *tok)
{
    char tmp[24];
    int len = tok->end - tok->start;
    if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, json + tok->start, len);
    tmp[len] = '\0';
    return strtoul(tmp, NULL, 10);
}

/* ── 标题清洗：去掉 TV动画/OST/片尾曲等括号后缀，提升搜索命中率 ──
 * "雲雀 (TV动画《...》片尾曲)" → "雲雀"；全角（）/《》/【】同样处理。
 * 就地截断：遇到第一个 '(' / '（' 即停。 */
static void strip_title_suffix(const char *in, char *out, int out_size)
{
    int o = 0;
    for (int i = 0; in[i] && o < out_size - 1; i++) {
        if (in[i] == '(' || in[i] == '（') break;
        /* 前导/尾随空格不拷贝由调用方 trim，这里只截断 */
        out[o++] = in[i];
    }
    /* 去尾随空格 */
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

/* ── ASCII 小写化（多候选打分用，非 ASCII 字符原样保留）── */
static void ascii_lower(const char *in, char *out, int out_size)
{
    int o = 0;
    for (int i = 0; in[i] && o < out_size - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[o] = '\0';
}

/* ── jsmn 容器宽度：返回容器 token（OBJ/ARR）结束后的下一个下标 ──
 * jsmn 的 size = 直接子 token 数（object: key+value 各算一个）。
 * 递归定义：宽度 = 1 + Σ(每个直接子 token 的宽度)；标量宽度 = 1。 */
static int jsmn_container_end(const jsmntok_t *tokens, int num, int idx)
{
    if (idx < 0 || idx >= num) return idx;
    jsmntype_t t = tokens[idx].type;
    if (t != JSMN_OBJECT && t != JSMN_ARRAY) return idx + 1;

    int width = 1;             /* 容器自身占 1 */
    for (int i = 0; i < tokens[idx].size; i++) {
        if (idx + width >= num) break;
        jsmntype_t ct = tokens[idx + width].type;
        if (ct == JSMN_OBJECT || ct == JSMN_ARRAY) {
            int sub_end = jsmn_container_end(tokens, num, idx + width);
            width += sub_end - (idx + width);
        } else {
            width += 1;
        }
    }
    return idx + width;
}

/* ── 解析 NetEase 搜索响应，多候选打分选最佳 songId ──
 * 响应结构: {result:{songs:[{id,name,...},...]}}
 * 打分: 100=歌名完全相等(忽略大小写), 60+长度比=前缀匹配, 40=包含, 0=兜底
 */
static unsigned long search_parse_best(const char *resp, const char *clean_title)
{
    /* limit=8 的搜索响应实测需要 ~700 token，512 会 JSMN_ERROR_NOMEM(-1)。
     * jsmntok_t 约 16 字节，1024 个 = 16KB，放在此任务自己的 PSRAM 栈上没问题
     * （lyric_fetch 任务栈 48KB PSRAM）。 */
    jsmn_parser parser;
    jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * 4096);
    if (!tokens) {
        ESP_LOGE(TAG, "token array alloc failed");
        return 0;
    }
    jsmn_init(&parser);
    int num = jsmn_parse(&parser, resp, strlen(resp), tokens, 4096);
    if (num < 0) {
        ESP_LOGE(TAG, "JSON parse error: %d", num);
        free(tokens);
        return 0;
    }

    /* result.songs[] — 必须在 result 对象的直接子 key 中找 songs，
     * 不能用线性扫描（会命中嵌套对象内部的同名 key） */
    int ri = json_find_key(resp, tokens, num, 0, "result");
    if (ri < 0 || tokens[ri].type != JSMN_OBJECT) { free(tokens); return 0; }
    int si = json_find_key_in_object(resp, tokens, num, ri, "songs");
    if (si < 0) { free(tokens); return 0; }
    ESP_LOGI(TAG, "ri=%d(ri_type=%d) si=%d(si_type=%d) si-1_type=%d si-1_str='%.*s'",
             ri, tokens[ri].type, si, tokens[si].type,
             si-1 >= 0 ? tokens[si-1].type : -1,
             si-1 >= 0 ? tokens[si-1].end - tokens[si-1].start : 0,
             si-1 >= 0 ? resp + tokens[si-1].start : "");
    if (tokens[si].type != JSMN_ARRAY || tokens[si].size < 1) {
        free(tokens);
        return 0;
    }

    /* 遍历 songs 数组，对每个元素找顶层 "id" 和 "name"
     * jsmn size 语义：容器的 size = 直接子 token 数（object 是 key+value 各算一个）。
     * 容器跳转必须用 jsmn_container_end 递归算宽度，累加 size 的旧算法对
     * cloudsearch 这类深嵌套响应会算错 obj_end → 解析出 id=0。 */
    int song_count = tokens[si].size;
    int pos = si + 1;  /* songs 数组第一个元素 */
    /* dump songs 数组第一个元素的原始 JSON */
    if (pos < num && tokens[pos].type == JSMN_OBJECT) {
        int dlen = tokens[pos].end - tokens[pos].start;
        if (dlen > 200) dlen = 200;
        ESP_LOGI(TAG, "songs[0] raw (%d): %.*s", dlen, dlen, resp + tokens[pos].start);
    } else if (pos < num) {
        ESP_LOGI(TAG, "songs[0] pos=%d type=%d, skip to %d", pos, tokens[pos].type, pos+1 < num ? tokens[pos+1].type : -1);
        /* 如果第一个 token 不是 OBJECT（可能是 ARRAY 自身），跳过 */
        if (tokens[pos].type == JSMN_ARRAY) pos++;
    }
    unsigned long best_id = 0;
    char best_name[64] = "";
    int best_score = -1;
    ESP_LOGI(TAG, "search_parse_best: %d candidates for '%.32s' (tokens=%d, resp_len=%d)",
             song_count, clean_title, num, (int)strlen(resp));
    if (song_count < 1) {
        ESP_LOGW(TAG, "no song candidate; resp head: %.120s", resp);
    }

    for (int s = 0; s < song_count && pos < num; s++) {
        if (tokens[pos].type != JSMN_OBJECT) { pos++; continue; }
        /* 字符边界法：jsmn 的 size 对嵌套容器不是可靠的子树长度 */
        int obj_end = pos + 1;
        while (obj_end < num && tokens[obj_end].start < tokens[pos].end) {
            obj_end++;
        }

        /* 在这个 object 内找顶层 "id" 和 "name"（只看直接子 key） */
        unsigned long song_id = 0;
        char song_name[64] = "";
        int scan = pos + 1;
        while (scan < obj_end && scan < num) {
            int klen = tokens[scan].end - tokens[scan].start;
            if (tokens[scan].type == JSMN_STRING) {
                /* 顶层 "id" key（值可以是 PRIMITIVE 数字或 STRING） */
                if (klen == 2 && memcmp(resp + tokens[scan].start, "id", 2) == 0
                    && scan + 1 < num) {
                    if (tokens[scan + 1].type == JSMN_PRIMITIVE) {
                        song_id = json_get_int(resp, &tokens[scan + 1]);
                    } else if (tokens[scan + 1].type == JSMN_STRING) {
                        char id_buf[32];
                        json_get_string(resp, &tokens[scan + 1], id_buf, sizeof(id_buf));
                        song_id = strtoul(id_buf, NULL, 10);
                    }
                }
                /* 顶层 "name" key（值必须是 STRING） */
                if (klen == 4 && memcmp(resp + tokens[scan].start, "name", 4) == 0
                    && scan + 1 < num && tokens[scan + 1].type == JSMN_STRING) {
                    json_get_string(resp, &tokens[scan + 1], song_name, sizeof(song_name));
                }
            }
            /* 跳过 key + value；value 是容器时按字符边界跳 */
            int value = scan + 1;
            scan += 2;
            while (scan < obj_end && scan < num
                   && tokens[scan].start < tokens[value].end) {
                scan++;
            }
        }

        ESP_LOGI(TAG, "song[%d]: id=%lu name='%s'", s, song_id, song_name);
        if (song_id > 0) {
            /* 打分：候选歌名与清洗后目标歌名的匹配度 */
            int score = 0;
            char cand[64], target[64];
            ascii_lower(song_name, cand, sizeof(cand));
            ascii_lower(clean_title, target, sizeof(target));
            if (cand[0] && target[0]) {
                if (strcmp(cand, target) == 0) {
                    score = 100;
                } else {
                    /* 候选含目标前缀（如 "Cage (WALL-G Edit)" 含 "cage"） */
                    size_t tl = strlen(target);
                    if (strncmp(cand, target, tl) == 0) {
                        score = 60 + (int)(tl * 40 / strlen(cand));
                    } else if (strstr(cand, target)) {
                        score = 40;
                    }
                }
            }
            if (best_id == 0 || score > best_score) {
                best_id = song_id;
                snprintf(best_name, sizeof(best_name), "%s", song_name);
                best_score = score;
            }
        }

        pos = obj_end;
    }

    if (best_id > 0) {
        ESP_LOGI(TAG, "Found: [%lu] %s (score=%d)", best_id, best_name, best_score);
    } else {
        ESP_LOGW(TAG, "Song not found in this endpoint");
    }
    free(tokens);
    return best_id;
}

/* ── 搜索歌曲（NetEase 两级），返回 songId，失败返回 0 ── */
static unsigned long search_song(const char *title, const char *artist)
{
    char clean_title[128];
    strip_title_suffix(title, clean_title, sizeof(clean_title));
    if (!clean_title[0]) {
        snprintf(clean_title, sizeof(clean_title), "%s", title ? title : "");
    }

    char keyword[128];
    snprintf(keyword, sizeof(keyword), "%s %s", clean_title, artist);

    char encoded[256];
    url_encode(keyword, encoded, sizeof(encoded));

    char post_data[320];
    /* 请求参数 limit=8：给多候选打分留足余量 */
    snprintf(post_data, sizeof(post_data),
             "s=%s&type=1&limit=8&offset=0", encoded);

    /* 第一级：老接口 */
    char *resp = http_request("http://music.163.com/api/search/get",
                              post_data, "application/x-www-form-urlencoded",
                              HTTP_SEARCH_BUF);
    if (resp) {
        ESP_LOGI(TAG, "search resp (%d bytes)", (int)strlen(resp));
        unsigned long id = search_parse_best(resp, clean_title);
        free(resp);
        if (id > 0) return id;
    } else {
        ESP_LOGW(TAG, "search_song: HTTP request failed");
    }

    /* 第二级：cloudsearch 新接口兜底（老接口空结果/HTTP失败时） */
    ESP_LOGI(TAG, "NetEase primary search miss, trying cloudsearch");
    resp = http_request_ex("https://music.163.com/api/cloudsearch/pc",
                           post_data, "application/x-www-form-urlencoded",
                           HTTP_SEARCH_BUF, "https://music.163.com");
    if (!resp) {
        ESP_LOGW(TAG, "cloudsearch HTTP failed");
        return 0;
    }
    ESP_LOGI(TAG, "cloudsearch resp (%d bytes)", (int)strlen(resp));
    unsigned long id = search_parse_best(resp, clean_title);
    free(resp);
    return id;
}

/* ── JSON 转义还原（\n → 换行等）── */
static void json_unescape(char *buf, int len)
{
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\\' && i + 1 < len && buf[i + 1] == 'n') {
            buf[j++] = '\n';
            i++;
        } else if (buf[i] == '\\' && i + 1 < len && buf[i + 1] == 'r') {
            buf[j++] = '\r';
            i++;
        } else {
            buf[j++] = buf[i];
        }
    }
    buf[j] = '\0';
}

/* ── 从 JSON 中提取指定字段的文本（调用方 free）── */
static char *extract_json_field(const char *resp, jsmntok_t *tokens, int num_tokens,
                                 const char *obj_key, const char *field_key)
{
    int idx = json_find_key(resp, tokens, num_tokens, 0, obj_key);
    if (idx < 0) return NULL;
    int fld = json_find_key(resp, tokens, num_tokens, idx, field_key);
    if (fld < 0) return NULL;
    int len = tokens[fld].end - tokens[fld].start;
    if (len <= 0) return NULL;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, resp + tokens[fld].start, len);
    out[len] = '\0';
    json_unescape(out, len);
    return out;
}

/* ── 获取 LRC + klyric 歌词文本（调用方 free 返回的 LRC）── */
static char *fetch_lrc(unsigned long song_id)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://music.163.com/api/song/lyric?id=%lu&lv=-1&kv=-1&tv=-1&yv=-1", song_id);

    char *resp = http_request(url, NULL, NULL, HTTP_LYRIC_BUF);
    if (!resp) {
        ESP_LOGW(TAG, "fetch_lrc: HTTP request failed for song_id=%lu", song_id);
        return NULL;
    }
    ESP_LOGI(TAG, "fetch_lrc: got response (%d bytes)", (int)strlen(resp));

    jsmn_parser parser;
    jsmntok_t *tokens = (jsmntok_t *)heap_caps_malloc(4096 * sizeof(jsmntok_t), MALLOC_CAP_SPIRAM);
    if (!tokens) {
        ESP_LOGE(TAG, "Failed to alloc jsmn tokens from PSRAM");
        free(resp);
        return NULL;
    }
    jsmn_init(&parser);
    int num = jsmn_parse(&parser, resp, strlen(resp), tokens, 4096);
    if (num < 0) {
        ESP_LOGE(TAG, "Lyric JSON parse error: %d", num);
        free(tokens);
        free(resp);
        return NULL;
    }

    char *lrc = extract_json_field(resp, tokens, num, "lrc", "lyric");
    if (!lrc) {
        ESP_LOGW(TAG, "No lrc field in response");
        free(tokens);
        free(resp);
        return NULL;
    }
    ESP_LOGI(TAG, "LRC extracted: %d bytes, first 80: '%.80s'", (int)strlen(lrc), lrc);

    static const char *klyric_keys[] = {"yrc", "klyric", "yrcxl", NULL};
    for (int kk = 0; klyric_keys[kk]; kk++) {
        char *klyric_raw = extract_json_field(resp, tokens, num, klyric_keys[kk], "lyric");
        if (klyric_raw && strlen(klyric_raw) > 10) {
            ESP_LOGI(TAG, "klyric(%s) extracted: %d bytes, first 80: '%.80s'",
                klyric_keys[kk], (int)strlen(klyric_raw), klyric_raw);
            parse_klyric(klyric_raw);
            free(klyric_raw);
            if (s_klyric_count > 0) break;  /* 有逐字数据才跳出 */
        } else if (klyric_raw) {
            ESP_LOGI(TAG, "klyric(%s) too short (%d bytes), skip", klyric_keys[kk], (int)strlen(klyric_raw));
            free(klyric_raw);
        }
    }

    free(tokens);
    free(resp);
    return lrc;
}

/* ── 从 LRC 行数据生成近似逐字时间戳（无 klyric 时的回退）──
 * 策略：按行时长均分到每个可唱字，标点不产生 word entry（避免与
 *       lyrics_get_karaoke_byte_idx 的标点跳过逻辑产生字数不匹配）。
 *       标点的时间权重分摊到同行可唱字中。 */
static void generate_pseudo_klyric(const lyric_data_t *data)
{
    if (s_klyric_count > 0 || !data || data->count < 2) return;

    ESP_LOGI(TAG, "Generating pseudo-klyric from %d LRC lines", data->count);

    int word_idx = 0;
    int line_idx = 0;

    for (int i = 0; i < data->count && word_idx < KLYRIC_MAX_WORDS - 1 && line_idx < LYRIC_MAX_LINES; i++) {
        const char *text = data->lines[i].text;
        if (!text[0]) continue;

        int line_start = data->lines[i].time_ms;
        /* 末句无下句时，按可唱字符数估算时长（参考 MeloX min/max 0.32s/字, 2~8s）
         * 避免短末句固定5秒长时间空滚 */
        int line_end;
        if (i + 1 < data->count) {
            line_end = data->lines[i + 1].time_ms;
        } else {
            int sung_chars = 0;
            const char *st = text;
            while (*st) {
                unsigned char c = (unsigned char)*st;
                if (c >= 0xE0) { sung_chars++; st += 3; }
                else if (c >= 0xC0) { sung_chars++; st += 2; }
                else { sung_chars++; st++; }
            }
            int est_ms = (int)(sung_chars * 320.0f);   /* 0.32s/字 */
            if (est_ms < 2000) est_ms = 2000;          /* 下限 2s */
            if (est_ms > 8000) est_ms = 8000;          /* 上限 8s */
            line_end = line_start + est_ms;
        }
        int line_dur = line_end - line_start;
        if (line_dur <= 0) line_dur = 3000;

        /* 计算可唱字符的总权重（用于分配时间） — 用统一判定 */
        float sung_weight = 0;
        const char *p = text;
        while (*p) {
            int bytes = 1;
            if (lyric_is_sung_char(p, &bytes)) {
                sung_weight += (bytes >= 2) ? 1.0f : 0.5f;
            }
            p += bytes;
        }
        if (sung_weight < 1) sung_weight = 1;

        /* 标点的时间分摊系数：总时长按可唱权重比例分配 */
        float time_per_sung_weight = (float)line_dur / sung_weight;

        s_klyric_line_first[line_idx] = word_idx;
        s_klyric_line_time[line_idx] = line_start;
        line_idx++;

        /* 逐字分配时间 — 只为可唱字符创建 word entry（统一判定） */
        int t = line_start;
        p = text;
        while (*p && word_idx < KLYRIC_MAX_WORDS - 1) {
            int char_bytes = 1;
            bool is_sung = lyric_is_sung_char(p, &char_bytes);

            if (!is_sung) {
                /* 标点不产生 word entry，只消耗一点时间让后续字延后（停顿感） */
                int punct_dur = (int)(0.3f * time_per_sung_weight);
                if (punct_dur < 30) punct_dur = 30;
                if (punct_dur > 300) punct_dur = 300;
                t += punct_dur;
            } else {
                float w = (char_bytes >= 2) ? 1.0f : 0.5f;
                int dur = (int)(w * time_per_sung_weight);
                if (dur < 50) dur = 50;
                s_klyric_start[word_idx] = t;
                s_klyric_end[word_idx] = t + dur;
                word_idx++;
                t += dur;
            }

            p += char_bytes;
        }
    }

    s_klyric_count = word_idx;
    s_klyric_line_count = line_idx;
    ESP_LOGI(TAG, "Pseudo-klyric: %d words across %d lines (punct skipped)", s_klyric_count, s_klyric_line_count);
    /* 前 5 行详情 */
    for (int li = 0; li < s_klyric_line_count && li < 5; li++) {
        int wf = s_klyric_line_first[li];
        int wl = (li + 1 < s_klyric_line_count) ? s_klyric_line_first[li + 1] : s_klyric_count;
        ESP_LOGI(TAG, "  pseudo[%d]: t=%d words=%d t0=%d txt=%.16s",
                 li, s_klyric_line_time[li], wl - wf,
                 wl > wf ? s_klyric_start[wf] : -1,
                 data->lines[li].text);
    }
}

/* ── 解析 LRC 格式 "[MM:SS.xx]text" ── */
static void parse_lrc(const char *lrc_text, lyric_data_t *data)
{
    data->count = 0;
    data->loaded = false;

    const char *p = lrc_text;
    while (*p && data->count < LYRIC_MAX_LINES) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        if (*p == '[') {
            int mm = 0, ss = 0, ms = 0;
            if (sscanf(p, "[%d:%d.%d]", &mm, &ss, &ms) >= 2) {
                const char *text_start = strchr(p, ']');
                if (!text_start) { p++; continue; }
                text_start++;

                int time_ms = mm * 60000 + ss * 1000;
                if (ms < 100) ms *= 10;
                time_ms += ms;

                char text[LYRIC_TEXT_LEN] = {0};
                int ti = 0;
                const char *t = text_start;
                while (*t && *t != '\n' && *t != '\r' && ti < LYRIC_TEXT_LEN - 1) {
                    text[ti++] = *t++;
                }
                text[ti] = '\0';

                if (ti > 0) {
                    /* 强过滤元数据行：作词/作曲/编曲/翻译/歌词来源等 */
                    static const char *meta_keys[] = {
                        "作词", "作曲", "编曲", "制作人", "监制",
                        "录音", "混音", "母带", "吉他", "Bass",
                        "架子鼓", "大提琴", "小提琴", "管弦", "工程师",
                        "出品", "发行", "公司", "solo", "Studio",
                        "词曲", "词：", "曲：", "Lyrics", "lyrics",
                        "Composer", "Arranger", "Producer", "Written",
                        "混缩", "和声", "OP:", "SP:", "OP：", "SP：",
                        "来源", "翻译", "译配", "改编",
                        NULL
                    };
                    bool is_meta = false;
                    for (int k = 0; meta_keys[k]; k++) {
                        if (strstr(text, meta_keys[k])) { is_meta = true; break; }
                    }
                    /* 含冒号的行（"词曲 : XXX"、"Lyrics by: XXX"） */
                    if (!is_meta && strchr(text, ':')) is_meta = true;
                    /* 纯空白或极短 */
                    if (!is_meta && ti <= 2) is_meta = true;
                    if (!is_meta) {
                        data->lines[data->count].time_ms = time_ms;
                        strncpy(data->lines[data->count].text, text, LYRIC_TEXT_LEN);
                        data->count++;
                    }
                }

                p = t;
                while (*p == '\n' || *p == '\r') p++;
                continue;
            }
        }

        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (data->count > 0) {
        data->loaded = true;
        ESP_LOGI(TAG, "Parsed %d lyric lines", data->count);
    }
}

/* ── 解析 klyric/yrc 逐字时间戳 ──
 * klyric 格式: [line_start,dur](offset,dur)word...  （offset 相对前一字末尾）
 * yrc 格式:    [line_start,dur](start,dur,flag)word...（start 是绝对毫秒）
 * 两者靠括号内参数个数区分：3 参 = yrc，2 参 = klyric。
 * 一旦本曲出现 3 参即锁定为 yrc，后续 2 参一律忽略。
 */
static void parse_klyric(const char *klyric_text)
{
    s_klyric_count = 0;
    s_klyric_line_count = 0;
    bool is_yrc = false;

    const char *p = klyric_text;
    while (*p && s_klyric_count < KLYRIC_MAX_WORDS && s_klyric_line_count < LYRIC_MAX_LINES) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        if (*p == '[') {
            int line_start = 0, line_dur = 0;
            if (sscanf(p, "[%d,%d]", &line_start, &line_dur) >= 2) {
                s_klyric_line_first[s_klyric_line_count] = s_klyric_count;
                s_klyric_line_time[s_klyric_line_count] = line_start;
                s_klyric_line_count++;

                const char *rp = strchr(p, ']');
                if (!rp) { p++; continue; }
                rp++;

                /* klyric 2-param 格式用 word_time 累加相对偏移 */
                int word_time = line_start;
                while (*rp && *rp != '\n' && *rp != '\r' && s_klyric_count < KLYRIC_MAX_WORDS) {
                    if (*rp != '(') { rp++; continue; }

                    int a = 0, b = 0, c = 0;
                    int n = sscanf(rp, "(%d,%d,%d)", &a, &b, &c);
                    if (n == 3) {
                        /* yrc 格式: (start,dur,flag) — start 是绝对毫秒 */
                        if (!is_yrc) {
                            is_yrc = true;
                            word_time = line_start;
                        }
                        /* dur<=0 时兜底（对齐 LyricOn: dur==0 → end-start，
                         * 保证每个字至少有点时长，karaoke 不会瞬间跳过） */
                        int end = a + b;
                        if (b <= 0) {
                            if (end > a) b = end - a;
                            else { b = 1; end = a + 1; }
                        }
                        s_klyric_start[s_klyric_count] = a;
                        s_klyric_end[s_klyric_count] = end;
                        s_klyric_count++;
                        rp = strchr(rp, ')');
                        if (!rp) break;
                        rp++;
                        while (*rp && *rp != '(' && *rp != '\n' && *rp != '\r') rp++;
                    } else if (sscanf(rp, "(%d,%d)", &a, &b) >= 2) {
                        /* klyric 格式: (offset,dur) — offset 相对前一字末尾 */
                        if (!is_yrc) {
                            word_time += a;
                            int end = word_time + b;
                            if (b <= 0) {   /* dur<=0 兜底，避免 0 时长字 */
                                if (end > word_time) b = end - word_time;
                                else { b = 1; end = word_time + 1; }
                            }
                            s_klyric_start[s_klyric_count] = word_time;
                            s_klyric_end[s_klyric_count] = word_time + b;
                            s_klyric_count++;
                            word_time += b;
                        }
                        /* yrc 中夹杂的 2 参数格式：跳过，不混入绝对时间轴 */
                        rp = strchr(rp, ')');
                        if (!rp) break;
                        rp++;
                        while (*rp && *rp != '(' && *rp != '\n' && *rp != '\r') rp++;
                    } else {
                        rp++;
                    }
                }
            }
        }

        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }

    ESP_LOGI(TAG, "Parsed %d klyric words across %d lines (yrc=%s)",
             s_klyric_count, s_klyric_line_count, is_yrc ? "yes" : "no");
}

/* ── 后台获取任务 ── */

static void reset_klyric(void)
{
    s_klyric_count = 0;
    s_klyric_line_count = 0;
    memset(s_lrc_to_klyric, -1, sizeof(s_lrc_to_klyric));
}

static int count_lrc_timestamps(const char *lrc)
{
    int n = 0;
    const char *p = lrc;
    while (p && *p) {
        int mm = 0, ss = 0;
        if (*p == '[' && sscanf(p, "[%d:%d", &mm, &ss) == 2) n++;
        p++;
    }
    return n;
}

static bool lrc_usable(const char *lrc)
{
    if (!lrc) return false;
    int stamps = count_lrc_timestamps(lrc);
    if (stamps < 2) return false;
    if (stamps < 5 &&
        (strstr(lrc, "\xe7\xba\xaf\xe9\x9f\xb3\xe4\xb9\x90") ||
         strstr(lrc, "\xe6\x9a\x82\xe6\x97\xa0") ||
         strstr(lrc, "\xe6\xb2\xa1\xe6\x9c\x89\xe5\xa1\xab\xe8\xaf\x8d") ||
         strstr(lrc, "nolyric"))) {
        return false;
    }
    return true;
}

static char *json_extract_string_alloc(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return NULL;
    p++;

    size_t cap = 256;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    while (*p && *p != '"') {
        char ch;
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') ch = '\n';
            else if (*p == 'r') ch = '\r';
            else if (*p == 't') ch = '\t';
            else ch = *p;
            p++;
        } else {
            ch = *p++;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[n++] = ch;
    }
    out[n] = '\0';
    return out;
}

static bool json_extract_quoted(const char *json, const char *key, char *out, int out_size)
{
    char *value = json_extract_string_alloc(json, key);
    if (!value) return false;
    snprintf(out, out_size, "%s", value);
    free(value);
    return out[0] != '\0';
}

static char *base64_decode_alloc(const char *src)
{
    if (!src || !src[0]) return NULL;
    size_t src_len = strlen(src);
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &out_len,
                                    (const unsigned char *)src, src_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && ret != 0) return NULL;
    if (out_len == 0) return NULL;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    ret = mbedtls_base64_decode((unsigned char *)out, out_len, &out_len,
                                (const unsigned char *)src, src_len);
    if (ret != 0) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    return out;
}

static char *normalize_lyric_payload(char *lyric)
{
    if (!lyric) return NULL;
    if (lrc_usable(lyric)) return lyric;
    char *decoded = base64_decode_alloc(lyric);
    free(lyric);
    return decoded;
}

static char *fetch_lrc_qq(const char *title, const char *artist)
{
    char clean_title[128];
    strip_title_suffix(title, clean_title, sizeof(clean_title));
    if (!clean_title[0]) {
        snprintf(clean_title, sizeof(clean_title), "%s", title ? title : "");
    }
    char keyword[128];
    char encoded[256];
    char url[512];
    snprintf(keyword, sizeof(keyword), "%s %s", clean_title, artist);
    url_encode(keyword, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&n=1&p=1&w=%s",
             encoded);

    char *resp = http_request_ex(url, NULL, NULL, HTTP_FALLBACK_SEARCH_BUF, "https://y.qq.com");
    if (!resp) {
        /* QQ 搜索偶发 HTTP 失败（连接被重置/DNS 抖动），重试一次 */
        vTaskDelay(pdMS_TO_TICKS(300));
        resp = http_request_ex(url, NULL, NULL, HTTP_FALLBACK_SEARCH_BUF, "https://y.qq.com");
    }
    if (!resp) {
        ESP_LOGW(TAG, "QQ search HTTP failed (retry exhausted)");
        return NULL;
    }

    char songmid[48] = {0};
    if (!json_extract_quoted(resp, "songmid", songmid, sizeof(songmid))) {
        json_extract_quoted(resp, "mid", songmid, sizeof(songmid));
    }
    free(resp);
    if (!songmid[0]) {
        ESP_LOGW(TAG, "QQ songmid not found");
        return NULL;
    }
    ESP_LOGI(TAG, "QQ songmid=%s", songmid);

    snprintf(url, sizeof(url),
             "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg"
             "?songmid=%s&format=json&nobase64=1&g_tk=5381&platform=yqq",
             songmid);
    resp = http_request_ex(url, NULL, NULL, HTTP_LYRIC_BUF, "https://y.qq.com");
    if (!resp) {
        ESP_LOGW(TAG, "QQ lyric HTTP failed");
        return NULL;
    }

    const char *json = strchr(resp, '{');
    if (!json) {
        free(resp);
        return NULL;
    }
    char *lyric = json_extract_string_alloc(json, "lyric");
    free(resp);
    lyric = normalize_lyric_payload(lyric);
    if (!lyric) ESP_LOGW(TAG, "QQ lyric empty");
    return lyric;
}

static char *kuwo_lrclist_to_lrc(const char *resp)
{
    const char *p = strstr(resp, "\"lrclist\"");
    if (!p) return NULL;

    char *out = malloc(HTTP_LYRIC_BUF);
    if (!out) return NULL;
    int oi = 0;
    out[0] = '\0';

    while ((p = strstr(p, "\"time\"")) != NULL) {
        p = strchr(p, ':');
        if (!p) break;
        p++;
        while (*p == ' ' || *p == '"') p++;
        char time_s[16];
        int ti = 0;
        while (*p && *p != '"' && *p != ',' && *p != '}' && ti < (int)sizeof(time_s) - 1) {
            time_s[ti++] = *p++;
        }
        time_s[ti] = '\0';

        const char *lp = strstr(p, "\"lineLyric\"");
        if (!lp) break;
        lp = strchr(lp, ':');
        if (!lp) break;
        lp++;
        while (*lp == ' ') lp++;
        if (*lp != '"') {
            p = lp;
            continue;
        }
        lp++;

        char line[LYRIC_TEXT_LEN];
        int li = 0;
        while (*lp && *lp != '"' && li < LYRIC_TEXT_LEN - 1) {
            if (*lp == '\\' && lp[1]) {
                lp++;
                if (*lp == 'n' || *lp == 'r') {
                    lp++;
                    break;
                }
                line[li++] = *lp++;
                continue;
            }
            line[li++] = *lp++;
        }
        line[li] = '\0';

        float sec = strtof(time_s, NULL);
        int total_ms = (int)(sec * 1000.0f + 0.5f);
        if (total_ms < 0) total_ms = 0;
        int mm = total_ms / 60000;
        int ss = (total_ms % 60000) / 1000;
        int cs = (total_ms % 1000) / 10;
        int n = snprintf(out + oi, HTTP_LYRIC_BUF - oi, "[%02d:%02d.%02d]%s\n",
                         mm, ss, cs, line);
        if (n < 0 || n >= HTTP_LYRIC_BUF - oi) break;
        oi += n;
        p = lp;
        if (HTTP_LYRIC_BUF - oi < 64) break;
    }

    if (oi == 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char *fetch_lrc_kuwo(const char *title, const char *artist)
{
    char clean_title[128];
    strip_title_suffix(title, clean_title, sizeof(clean_title));
    if (!clean_title[0]) {
        snprintf(clean_title, sizeof(clean_title), "%s", title ? title : "");
    }
    char keyword[128];
    char encoded[256];
    char url[512];
    snprintf(keyword, sizeof(keyword), "%s %s", clean_title, artist);
    url_encode(keyword, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "http://search.kuwo.cn/r.s?all=%s&ft=music&itemset=web_2013&client=kt"
             "&pn=0&rn=1&rformat=json&encoding=utf8",
             encoded);

    char *resp = http_request_ex(url, NULL, NULL, HTTP_FALLBACK_SEARCH_BUF, "http://www.kuwo.cn");
    if (!resp) {
        ESP_LOGW(TAG, "Kuwo search HTTP failed");
        return NULL;
    }

    char music_id[24] = {0};
    const char *rid = strstr(resp, "MUSIC_");
    if (rid) {
        rid += 6;
        size_t n = 0;
        while (rid[n] >= '0' && rid[n] <= '9' && n + 1 < sizeof(music_id)) {
            music_id[n] = rid[n];
            n++;
        }
        music_id[n] = '\0';
    }
    free(resp);
    if (!music_id[0]) {
        ESP_LOGW(TAG, "Kuwo MUSICRID not found");
        return NULL;
    }
    ESP_LOGI(TAG, "Kuwo musicId=%s", music_id);

    snprintf(url, sizeof(url),
             "http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=%s", music_id);
    resp = http_request_ex(url, NULL, NULL, HTTP_LYRIC_BUF, "http://m.kuwo.cn");
    if (!resp) {
        ESP_LOGW(TAG, "Kuwo lyric HTTP failed");
        return NULL;
    }
    char *lrc = kuwo_lrclist_to_lrc(resp);
    free(resp);
    if (!lrc) ESP_LOGW(TAG, "Kuwo lrclist empty");
    return lrc;
}

static bool apply_lrc(char *lrc, lyric_data_t *data, uint32_t gen)
{
    if (!lrc) return false;
    if (!lrc_usable(lrc)) {
        ESP_LOGW(TAG, "Lyrics payload unusable (%d bytes)", (int)strlen(lrc));
        free(lrc);
        if (gen == s_fetch_gen) reset_klyric();
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (gen != s_fetch_gen) {
        xSemaphoreGive(s_mutex);
        free(lrc);
        ESP_LOGI(TAG, "Drop stale lyrics apply (gen %u != %u)", (unsigned)gen, (unsigned)s_fetch_gen);
        return false;
    }
    parse_lrc(lrc, data);
    if (data->count <= 0) {
        xSemaphoreGive(s_mutex);
        free(lrc);
        reset_klyric();
        return false;
    }

    if (s_klyric_count == 0 && data->count > 0) {
        generate_pseudo_klyric(data);
    }
    memset(s_lrc_to_klyric, -1, sizeof(s_lrc_to_klyric));
    if (s_klyric_line_count > 0 && data->count > 0) {
        int ki = 0;
        int mapped = 0, unmapped = 0;
        for (int li = 0; li < data->count; li++) {
            int lrc_time = data->lines[li].time_ms;
            /* 每行独立找时间最接近的 klyric 行，不再用全局递增的 ki。
             * 真实 yrc 常含翻译行/空行导致行数不等，贪心递增一旦错过一行，
             * 后续会整体错位一格——这正是"逐字高亮快了一句"的来源。
             * 只向前搜索（klyric 行不倒退），保证单调性。 */
            while (ki < s_klyric_line_count - 1 &&
                   abs(s_klyric_line_time[ki + 1] - lrc_time) < abs(s_klyric_line_time[ki] - lrc_time)) {
                ki++;
            }
            /* 若当前 ki 已明显超前（差值过大），回退到本行最优解 */
            if (ki > 0 && abs(s_klyric_line_time[ki] - lrc_time) > 100) {
                int best = ki;
                int best_diff = abs(s_klyric_line_time[ki] - lrc_time);
                for (int kj = ki - 1; kj >= 0; kj--) {
                    int d = abs(s_klyric_line_time[kj] - lrc_time);
                    if (d < best_diff) { best_diff = d; best = kj; }
                }
                if (best_diff <= 100) ki = best;
            }
            if (ki < s_klyric_line_count && abs(s_klyric_line_time[ki] - lrc_time) <= 100) {
                s_lrc_to_klyric[li] = ki;
                mapped++;
            } else {
                unmapped++;
            }
        }
        ESP_LOGI(TAG, "LRC→klyric mapping: %d mapped, %d unmapped (LRC=%d klyric=%d)",
                 mapped, unmapped, data->count, s_klyric_line_count);
        /* 前 5 行映射详情 */
        for (int li = 0; li < data->count && li < 5; li++) {
            int ki = s_lrc_to_klyric[li];
            ESP_LOGI(TAG, "  map[%d]: lrc_t=%d → kline=%d kt=%d txt=%.16s",
                     li, data->lines[li].time_ms, ki,
                     ki >= 0 ? s_klyric_line_time[ki] : -1,
                     data->lines[li].text);
        }
    }
    ESP_LOGI(TAG, "Applied %d lyric lines: first=%dms last=%dms '%.24s' ... '%.24s'",
             data->count, data->lines[0].time_ms,
             data->lines[data->count - 1].time_ms,
             data->lines[0].text, data->lines[data->count - 1].text);
    xSemaphoreGive(s_mutex);
    free(lrc);
    return true;
}

static void fetch_task(void *arg)
{
    (void)arg;
    lyric_data_t *data = &s_lyric_data;

    for (;;) {
        char title[64], artist[64];
        unsigned long known_id;
        uint32_t gen;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snprintf(title, sizeof(title), "%s", data->song_name);
        snprintf(artist, sizeof(artist), "%s", data->artist);
        known_id = data->known_song_id;
        gen = s_fetch_gen;
        reset_klyric();
        xSemaphoreGive(s_mutex);

        if (!title[0]) {
            ESP_LOGI(TAG, "Fetch cancelled, empty title");
            goto exit_or_retry;
        }

        char *lrc = NULL;
        if (known_id > 0) {
            ESP_LOGI(TAG, "Using known songId=%lu for: %s - %s", known_id, title, artist);
            lrc = fetch_lrc(known_id);
            if (gen != s_fetch_gen) {
                free(lrc);
                continue;
            }
            if (apply_lrc(lrc, data, gen)) {
                ESP_LOGI(TAG, "Lyrics from NetEase songId");
                goto exit_or_retry;
            }
            ESP_LOGW(TAG, "Known songId failed, search NetEase by title");
        }

        if (gen != s_fetch_gen) continue;
        /* 打印清洗后的实际关键词，便于核对搜索行为 */
        {
            char log_title[96];
            strip_title_suffix(title, log_title, sizeof(log_title));
            ESP_LOGI(TAG, "Searching NetEase lyrics: %s - %s",
                     log_title[0] ? log_title : "", artist);
        }
        {
            unsigned long song_id = search_song(title, artist);
            if (gen != s_fetch_gen) continue;
            if (song_id > 0) {
                lrc = fetch_lrc(song_id);
                if (gen != s_fetch_gen) {
                    free(lrc);
                    continue;
                }
                if (apply_lrc(lrc, data, gen)) {
                    ESP_LOGI(TAG, "Lyrics from NetEase search");
                    goto exit_or_retry;
                }
            } else {
                ESP_LOGW(TAG, "NetEase song not found");
            }
        }

        if (gen != s_fetch_gen) continue;
        ESP_LOGI(TAG, "Netease lyrics not found, fallback to QQ");
        lrc = fetch_lrc_qq(title, artist);
        if (gen != s_fetch_gen) {
            free(lrc);
            continue;
        }
        if (apply_lrc(lrc, data, gen)) {
            ESP_LOGI(TAG, "Lyrics from QQ");
            goto exit_or_retry;
        }

        if (gen != s_fetch_gen) continue;
        ESP_LOGI(TAG, "QQ lyrics not found, fallback to Kuwo");
        lrc = fetch_lrc_kuwo(title, artist);
        if (gen != s_fetch_gen) {
            free(lrc);
            continue;
        }
        if (apply_lrc(lrc, data, gen)) {
            ESP_LOGI(TAG, "Lyrics from Kuwo");
            goto exit_or_retry;
        }

        if (gen == s_fetch_gen) {
            ESP_LOGW(TAG, "No lyrics available from NetEase/QQ/Kuwo");
        }

exit_or_retry:
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (gen != s_fetch_gen) {
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Song changed during fetch, retry");
            continue;
        }
        s_fetch_task = NULL;
        xSemaphoreGive(s_mutex);
        lyrics_delete_current_task();
        return;
    }
}

esp_err_t lyrics_init(void)
{
    if (s_mutex) return ESP_OK;  /* 已初始化 */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    ESP_LOGI(TAG, "Lyrics component initialized");
    return ESP_OK;
}

void lyrics_fetch_async(const char *title, const char *artist, unsigned long song_id)
{
    ESP_LOGI(TAG, "lyrics_fetch_async called: '%s' - '%s' songId=%lu", title, artist, song_id);
    if (!s_mutex) lyrics_init();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 缓存命中：歌名+歌手相同且已加载 */
    if (s_lyric_data.loaded &&
        strcmp(s_lyric_data.song_name, title) == 0 &&
        strcmp(s_lyric_data.artist, artist) == 0) {
        ESP_LOGI(TAG, "Lyrics cached for: %s - %s", title, artist);
        xSemaphoreGive(s_mutex);
        return;
    }

    /* 清除旧歌词 */
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    reset_klyric();
    strncpy(s_lyric_data.song_name, title, sizeof(s_lyric_data.song_name) - 1);
    strncpy(s_lyric_data.artist, artist, sizeof(s_lyric_data.artist) - 1);
    s_lyric_data.known_song_id = song_id;
    s_fetch_gen++;
    bool need_create = (s_fetch_task == NULL);
    xSemaphoreGive(s_mutex);

    /* 如果已有任务在跑，不重复创建 */
    if (!need_create) {
        ESP_LOGI(TAG, "Fetch in progress, reuse task for new song (gen=%u)", (unsigned)s_fetch_gen);
        return;
    }

    if (lyrics_create_task(fetch_task, "lyric_fetch", LYRIC_TASK_STACK_SIZE,
                           NULL, 3, &s_fetch_task, 1) == pdPASS) {
        ESP_LOGI(TAG, "Fetch task created (PSRAM stack, core 1)");
    } else {
        ESP_LOGE(TAG, "Failed to create lyrics fetch task");
    }
}

int lyrics_get_current_line(int position_ms)
{
    if (!s_mutex) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_lyric_data.loaded || s_lyric_data.count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 线性扫描：找最后一行 time_ms <= position_ms（参考 LrcView 写法） */
    int result = -1;
    for (int i = 0; i < s_lyric_data.count; i++) {
        if (s_lyric_data.lines[i].time_ms <= position_ms) {
            result = i;
        }
    }

    xSemaphoreGive(s_mutex);
    return result;
}

const lyric_data_t *lyrics_get_data(void)
{
    return &s_lyric_data;
}

int lyrics_get_karaoke_progress(int line_start_ms, int pos_ms)
{
    if (!s_mutex) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_klyric_line_count == 0 || s_klyric_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 按行起始时间匹配 klyric 行，不依赖行索引（LRC/klyric 行数可能不一致） */
    int kline = -1;
    for (int i = 0; i < s_klyric_line_count; i++) {
        if (s_klyric_line_time[i] <= line_start_ms + 50) {
            kline = i;
        }
    }
    if (kline < 0 || abs(s_klyric_line_time[kline] - line_start_ms) > 100) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    int first = s_klyric_line_first[kline];
    int last = (kline + 1 < s_klyric_line_count)
        ? s_klyric_line_first[kline + 1]
        : s_klyric_count;
    int word_count = last - first;
    if (word_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 二分查找当前所处的字 */
    int lo = first, hi = last - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_klyric_end[mid] <= pos_ms) {
            lo = mid + 1;
        } else {
            found = mid;
            hi = mid - 1;
        }
    }

    int progress;
    if (found < 0) {
        progress = 100;
    } else if (pos_ms < s_klyric_start[found]) {
        int idx = found - first;
        progress = (idx > 0) ? idx * 100 / word_count : 0;
    } else {
        int idx = found - first;
        int word_dur = s_klyric_end[found] - s_klyric_start[found];
        if (word_dur <= 0) {
            progress = (idx + 1) * 100 / word_count;
        } else {
            int sub = (pos_ms - s_klyric_start[found]) * 100 / word_dur;
            if (sub > 100) sub = 100;
            progress = (idx * 100 + sub) / word_count;
        }
    }

    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    xSemaphoreGive(s_mutex);
    return progress;
}

/* ── 获取逐字高亮的精确字节偏移（百分比映射 + 字间插值）──
 * 核心思路：用 klyric 百分比映射到 LRC 全部字符，不依赖 word_count 一致性 */
int lyrics_get_karaoke_byte_idx(int line_idx, const char *line_text, int pos_ms)
{
    if (!s_mutex || !line_text || !line_text[0] || line_idx < 0) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_klyric_line_count == 0 || s_klyric_count == 0 || line_idx >= LYRIC_MAX_LINES) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 直接查映射表 */
    int kline = s_lrc_to_klyric[line_idx];
    if (kline < 0 || kline >= s_klyric_line_count) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    int first = s_klyric_line_first[kline];
    int last = (kline + 1 < s_klyric_line_count)
        ? s_klyric_line_first[kline + 1]
        : s_klyric_count;
    int word_count = last - first;
    if (word_count == 0) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    /* 移植 MeloX 语义：不用百分比二次映射，直接看逐字时间戳决定"已唱到第几个字"。
     * 每个字独立的 [start,end]，pos_ms 进入该字区间即点亮该字。
     * 已唱字数 = 处于或越过本行第几个字。 */
    int sung_words;
    int found = -1;
    for (int k = first; k < last; k++) {
        if (pos_ms >= s_klyric_start[k]) {
            found = k;
        } else {
            break;
        }
    }
    sung_words = (found < 0) ? 0 : (found - first + 1);
    if (sung_words > word_count) sung_words = word_count;

    int first_word_start = (word_count > 0) ? s_klyric_start[first] : -1;
    xSemaphoreGive(s_mutex);

    /* 统计 line_text 中的可唱字符总数（统一判定） */
    int total_sung = 0;
    const char *t = line_text;
    while (*t) {
        int bytes = 1;
        if (lyric_is_sung_char(t, &bytes)) total_sung++;
        t += bytes;
    }

    /* 逐字严格对应：一个字 = 一个可唱字符（klyric 每个括号一个字）。
     * 已唱字数即已点亮字符数。当前正唱到的字也视为亮起半个取其整，
     * 让高亮覆盖到"正在唱"的那个字。 */
    int target_char = sung_words;
    if (target_char > total_sung) target_char = total_sung;

    /* 诊断日志：仅在字数变化时打印（避免 25Hz 刷屏） */
    static int s_last_sung = -1, s_last_line = -1;
    if (sung_words != s_last_sung || line_idx != s_last_line) {
        ESP_LOGI("KLIDX", "line=%d kline=%d wc=%d/%d sw=%d tgt=%d pos=%d t0=%d txt=%.16s",
                 line_idx, kline, word_count, total_sung, sung_words,
                 target_char, pos_ms, first_word_start, line_text);
        s_last_sung = sung_words;
        s_last_line = line_idx;
    }

    const char *p = line_text;
    int char_count = 0;
    while (*p && char_count < target_char) {
        int bytes = 1;
        if (lyric_is_sung_char(p, &bytes)) char_count++;
        p += bytes;
    }

    return (int)(p - line_text);
}

void lyrics_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_lyric_data, 0, sizeof(s_lyric_data));
    reset_klyric();
    s_fetch_gen++;  /* invalidate in-flight fetch */
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Lyrics cleared");
}
