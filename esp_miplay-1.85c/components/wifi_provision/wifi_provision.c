#include "wifi_provision.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WIFI_PROVISION";

#define WIFI_PROVISION_NVS_NAMESPACE "wifi_cfg"
#define WIFI_PROVISION_DNS_PORT 53
#define WIFI_PROVISION_AP_IP "192.168.4.1"
#define WIFI_PROVISION_DNS_STACK_BYTES   (12U * 1024U)
#define WIFI_PROVISION_HTTP_STACK_BYTES  (16U * 1024U)

static void wifi_provision_delete_current_task(void)
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

static BaseType_t wifi_provision_create_task(TaskFunction_t task, const char *name,
                                             uint32_t stack_bytes, void *arg,
                                             UBaseType_t priority, TaskHandle_t *handle)
{
    const uint32_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    if (handle) *handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        task, name, stack_bytes, arg, priority, handle,
        tskNO_AFFINITY, stack_caps);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "Task %s created with PSRAM stack (%u bytes)",
                 name, (unsigned)stack_bytes);
        return ret;
    }

    ESP_LOGW(TAG, "Task %s PSRAM stack creation failed; trying internal stack",
             name);
    return xTaskCreatePinnedToCore(task, name, stack_bytes, arg,
                                   priority, handle, tskNO_AFFINITY);
}

static volatile bool s_running;
static wifi_mode_t s_previous_mode = WIFI_MODE_STA;
static volatile wifi_provision_status_t s_status = WIFI_PROVISION_STATUS_IDLE;
static char s_ap_ssid[33];
static char s_target_ssid[33];
static char s_target_password[65];
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_connect_task;
static volatile int s_dns_sock = -1;
static volatile bool s_expected_disconnect;

static const char s_wifi_html[] =
    "<!doctype html>"
    "<html lang=\"zh-CN\">"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
    "<title>设备联网配置</title>"
    "<style>"
    ":root{--primary:#2563eb;--primary-hover:#1d4ed8;--bg:#f8fafc;--card:#ffffff;--text:#0f172a;--sub:#64748b;--border:#e2e8f0;--ring:rgba(37,99,235,0.15);--success:#10b981;--error:#ef4444}"
    "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,\"PingFang SC\",\"Hiragino Sans GB\",\"Microsoft YaHei\",sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:24px 16px}"
    ".container{width:100%;max-width:420px}"    ".header{text-align:center;margin-bottom:20px}"    ".logo-wrap{width:56px;height:56px;margin:0 auto 12px;background:linear-gradient(135deg,#3b82f6,#1d4ed8);border-radius:16px;display:flex;align-items:center;justify-content:center;box-shadow:0 8px 16px -4px rgba(37,99,235,0.3)}"    ".logo-wrap svg{width:30px;height:30px;fill:#fff}"    "h1{font-size:20px;font-weight:700;letter-spacing:-0.3px;color:var(--text);margin-bottom:4px}"    "p.sub{font-size:13px;color:var(--sub)}"    ".card{background:var(--card);border-radius:18px;padding:20px;box-shadow:0 4px 20px -2px rgba(0,0,0,0.05),0 1px 3px rgba(0,0,0,0.03);border:1px solid rgba(226,232,240,0.8);margin-bottom:16px}"    ".section-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}"    ".section-title{font-size:14px;font-weight:600;color:var(--sub);text-transform:uppercase;letter-spacing:0.5px}"    ".btn-refresh{background:none;border:none;color:var(--primary);font-size:13px;font-weight:500;cursor:pointer;display:inline-flex;align-items:center;gap:4px;padding:4px 8px;border-radius:6px;transition:background .2s}"    ".btn-refresh:hover{background:var(--ring)}"    ".btn-refresh svg{width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"    ".btn-refresh.spinning svg{animation:spin 1s linear infinite}"    "@keyframes spin{to{transform:rotate(360deg)}}"    ".wifi-list{max-height:190px;overflow-y:auto;border:1px solid var(--border);border-radius:12px;list-style:none;margin-bottom:16px;overscroll-behavior:contain}"    ".wifi-list:empty::before{content:\"点击右上角刷新以扫描附近 Wi-Fi\";display:block;padding:24px;text-align:center;font-size:13px;color:var(--sub)}"    ".wifi-item{display:flex;align-items:center;padding:11px 14px;border-bottom:1px solid #f1f5f9;cursor:pointer;transition:all .15s}"    ".wifi-item:last-child{border-bottom:none}"    ".wifi-item:hover,.wifi-item:active{background:#f8fafc}"    ".wifi-item.selected{background:#eff6ff;color:var(--primary);font-weight:600}"    ".wifi-name{flex:1;font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-right:10px}"    ".wifi-meta{display:flex;align-items:center;gap:8px}"    ".wifi-bars{display:flex;align-items:flex-end;gap:1.5px;height:13px;width:15px}"    ".wifi-bars span{width:2.5px;background:#cbd5e1;border-radius:1px;transition:background .2s}"    ".wifi-bars span.on{background:currentColor}"    ".wifi-bars span:nth-child(1){height:25%}"    ".wifi-bars span:nth-child(2){height:50%}"    ".wifi-bars span:nth-child(3){height:75%}"    ".wifi-bars span:nth-child(4){height:100%}"    ".field-group{margin-bottom:14px}"    ".field-label{display:block;font-size:13px;font-weight:500;color:var(--text);margin-bottom:6px}"    ".input-wrap{position:relative;display:flex;align-items:center}"    "input[type=\"text\"],input[type=\"password\"]{width:100%;height:44px;padding:0 14px;font-size:15px;background:#f8fafc;border:1px solid var(--border);border-radius:10px;outline:none;color:var(--text);transition:all .2s}"    "input[type=\"text\"]:focus,input[type=\"password\"]:focus{background:#fff;border-color:var(--primary);box-shadow:0 0 0 3px var(--ring)}"    ".eye-btn{position:absolute;right:8px;top:50%;transform:translateY(-50%);background:none;border:none;padding:6px;cursor:pointer;color:var(--sub);border-radius:6px;display:flex}"    ".eye-btn:hover{color:var(--text)}"    ".eye-btn svg{width:18px;height:18px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"    ".btn-primary{width:100%;height:46px;background:var(--primary);color:#fff;border:none;border-radius:11px;font-size:15px;font-weight:600;cursor:pointer;box-shadow:0 4px 12px rgba(37,99,235,0.25);transition:all .2s;display:flex;align-items:center;justify-content:center;gap:8px}"    ".btn-primary:hover{background:var(--primary-hover)}"    ".btn-primary:disabled{background:#94a3b8;box-shadow:none;cursor:not-allowed}"    ".status-box{margin-top:14px;padding:12px 14px;border-radius:10px;font-size:13px;line-height:1.5;display:none;align-items:center;gap:8px}"    ".status-box.show{display:flex}"    ".status-box.info{background:#eff6ff;color:#1d4ed8;border:1px solid #bfdbfe}"    ".status-box.success{background:#ecfdf5;color:#065f46;border:1px solid #a7f3d0}"    ".status-box.error{background:#fef2f2;color:#991b1b;border:1px solid #fecaca}"    ".spinner{width:14px;height:14px;border:2px solid currentColor;border-top-color:transparent;border-radius:50%;animation:spin .8s linear infinite;flex-shrink:0}"    ".footer{text-align:center;font-size:12px;color:#94a3b8;margin-top:12px}"    "</style>"    "</head>"    "<body>"    "<div class=\"container\">"    "<div class=\"header\">"    "<div class=\"logo-wrap\">"    "<svg viewBox=\"0 0 24 24\"><path d=\"M12 3c-4.97 0-9.47 2.02-12.73 5.27l1.41 1.41C3.32 7.04 7.42 5.2 12 5.2c4.58 0 8.68 1.84 11.32 4.48l1.41-1.41C21.47 5.02 16.97 3 12 3zm0 4.8c-3.64 0-6.94 1.48-9.33 3.87l1.41 1.41C5.87 11.29 8.76 10 12 10s6.13 1.29 7.92 3.08l1.41-1.41C18.94 9.28 15.64 7.8 12 7.8zm0 4.8c-2.32 0-4.42.94-5.94 2.46l1.41 1.41C8.5 15.44 10.15 14.6 12 14.6s3.5.84 4.53 1.87l1.41-1.41C16.42 13.54 14.32 12.6 12 12.6zm0 4.8c-1.1 0-2 .9-2 2s.9 2 2 2 2-.9 2-2-.9-2-2-2z\"/></svg>"    "</div>"    "<h1>设备联网配置</h1>"    "<p class=\"sub\">连接附近 Wi-Fi，让设备畅享在线音乐</p>"    "</div>"    "<div class=\"card\">"    "<div class=\"section-head\">"    "<span class=\"section-title\">可用 Wi-Fi 列表</span>"    "<button class=\"btn-refresh\" id=\"refreshBtn\" type=\"button\">"    "<svg viewBox=\"0 0 24 24\"><path d=\"M21.5 2v6h-6M2.5 22v-6h6M2 11.5a10 10 0 0 1 18.8-4.3M22 12.5a10 10 0 0 1-18.8 4.2\"/></svg>"    "<span id=\"refreshText\">刷新</span>"    "</button>"    "</div>"    "<ul class=\"wifi-list\" id=\"wifiList\"></ul>"    "<div class=\"field-group\">"    "<label class=\"field-label\" for=\"ssid\">网络名称 (SSID)</label>"    "<div class=\"input-wrap\">"    "<input type=\"text\" id=\"ssid\" placeholder=\"选择上方列表或直接输入\" autocomplete=\"off\">"    "</div>"    "</div>"    "<div class=\"field-group\">"    "<label class=\"field-label\" for=\"pwd\">Wi-Fi 密码</label>"    "<div class=\"input-wrap\">"    "<input type=\"password\" id=\"pwd\" placeholder=\"开放网络可留空\" autocomplete=\"off\">"    "<button type=\"button\" class=\"eye-btn\" id=\"togglePwd\" title=\"显示/隐藏密码\">"    "<svg id=\"eyeIcon\" viewBox=\"0 0 24 24\"><path d=\"M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>"    "</button>"    "</div>"    "</div>"    "<button type=\"button\" class=\"btn-primary\" id=\"connectBtn\">"    "<span id=\"btnText\">立即连接</span>"    "</button>"    "<div class=\"status-box\" id=\"statusBox\">"    "<div class=\"spinner\" id=\"spinner\"></div>"    "<span id=\"statusMsg\"></span>"    "</div>"    "</div>"    "<div class=\"footer\">仅支持 2.4GHz Wi-Fi 网络</div>"    "</div>"    "<script>"    "(function(){"    "var list=document.getElementById('wifiList'),"    "ssidInput=document.getElementById('ssid'),"    "pwdInput=document.getElementById('pwd'),"    "refreshBtn=document.getElementById('refreshBtn'),"    "refreshText=document.getElementById('refreshText'),"    "togglePwd=document.getElementById('togglePwd'),"    "connectBtn=document.getElementById('connectBtn'),"    "btnText=document.getElementById('btnText'),"    "statusBox=document.getElementById('statusBox'),"    "spinner=document.getElementById('spinner'),"    "statusMsg=document.getElementById('statusMsg');"    "var isScanning=false;"    "function showStatus(type, msg, showSpin){"    "statusBox.className = 'status-box show ' + type;"    "statusMsg.textContent = msg;"    "spinner.style.display = showSpin ? 'block' : 'none';"    "}"    "function hideStatus(){"    "statusBox.className = 'status-box';"    "}"    "function getBars(rssi){"    "var level = 1;"    "if(rssi >= -60) level = 4;"    "else if(rssi >= -70) level = 3;"    "else if(rssi >= -80) level = 2;"    "var h = '<div class=\"wifi-bars\">';"    "for(var i=1; i<=4; i++){"    "h += '<span class=\"' + (i<=level ? 'on' : '') + '\"></span>';"    "}"    "h += '</div>';"    "return h;"    "}"    "function scan(){"    "if(isScanning) return;"    "isScanning = true;"    "refreshBtn.classList.add('spinning');"    "refreshText.textContent = '扫描中';"    "list.innerHTML = '';"    "fetch('/api/wifi/scan')"    ".then(function(res){ return res.json(); })"    ".then(function(aps){"    "isScanning = false;"    "refreshBtn.classList.remove('spinning');"    "refreshText.textContent = '刷新';"    "if(!aps || !aps.length){"    "list.innerHTML = '<li style=\"padding:16px;text-align:center;font-size:13px;color:#94a3b8\">未找到附近 Wi-Fi，请点击刷新</li>';"    "return;"    "}"    "var seen = {};"    "var filtered = [];"    "for(var i=0; i<aps.length; i++){"    "var ap = aps[i];"    "if(!ap.ssid || seen[ap.ssid]) continue;"    "seen[ap.ssid] = true;"    "filtered.push(ap);"    "}"    "filtered.sort(function(a,b){ return b.rssi - a.rssi; });"    "list.innerHTML = '';"    "filtered.forEach(function(ap){"    "var li = document.createElement('li');"    "li.className = 'wifi-item' + (ssidInput.value === ap.ssid ? ' selected' : '');"    "li.innerHTML = '<span class=\"wifi-name\">' + escapeHtml(ap.ssid) + '</span>' +"    "'<div class=\"wifi-meta\">' + getBars(ap.rssi) + '</div>';"    "li.onclick = function(){"    "var prev = list.querySelector('.selected');"    "if(prev) prev.classList.remove('selected');"    "li.classList.add('selected');"    "ssidInput.value = ap.ssid;"    "pwdInput.focus();"    "};"    "list.appendChild(li);"    "});"    "})"    ".catch(function(){"    "isScanning = false;"    "refreshBtn.classList.remove('spinning');"    "refreshText.textContent = '重试';"    "showStatus('error', '扫描周围 Wi-Fi 失败，请重试', false);"    "});"    "}"    "function escapeHtml(str){"    "return str.replace(/[&<>\"']/g, function(m){"    "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[m];"    "});"    "}"    "togglePwd.onclick = function(){"    "if(pwdInput.type === 'password'){"    "pwdInput.type = 'text';"    "togglePwd.innerHTML = '<svg viewBox=\"0 0 24 24\"><path d=\"M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24M1 1l22 22\"/></svg>';"    "} else {"    "pwdInput.type = 'password';"    "togglePwd.innerHTML = '<svg viewBox=\"0 0 24 24\"><path d=\"M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>';"    "}"    "};"    "function pollStatus(){"    "fetch('/api/wifi/status')"    ".then(function(res){ return res.json(); })"    ".then(function(data){"    "if(data.status === 'connecting'){"    "showStatus('info', '设备正在连接网络，请稍候...', true);"    "setTimeout(pollStatus, 1200);"    "} else if(data.status === 'success'){"    "showStatus('success', '联网成功！设备即将完成设置', false);"    "btnText.textContent = '连接成功';"    "connectBtn.style.background = '#10b981';"    "} else if(data.status === 'failed'){"    "showStatus('error', '连接失败，请检查密码是否正确后重试', false);"    "connectBtn.disabled = false;"    "btnText.textContent = '立即连接';"    "}"    "})"    ".catch(function(){"    "showStatus('info', '配网指令已生效，设备正在切换网络，请查看设备状态', false);"    "btnText.textContent = '已发送配置';"    "});"    "}"    "connectBtn.onclick = function(){"    "var s = ssidInput.value.trim();"    "if(!s){"    "showStatus('error', '请先输入或选择 Wi-Fi 名称', false);"    "ssidInput.focus();"    "return;"    "}"    "connectBtn.disabled = true;"    "btnText.textContent = '正在连接...';"    "showStatus('info', '正在发送 Wi-Fi 配置到设备...', true);"    "fetch('/api/wifi/connect', {"    "method: 'POST',"    "headers: {'Content-Type': 'application/json'},"    "body: JSON.stringify({ssid: s, password: pwdInput.value})"    "})"    ".then(function(res){"    "if(!res.ok) throw new Error('status ' + res.status);"    "return res.json();"    "})"    ".then(function(){"    "pollStatus();"    "})"    ".catch(function(){"    "showStatus('error', '配置发送失败，请确认是否仍连接在配网热点', false);"    "connectBtn.disabled = false;"    "btnText.textContent = '立即连接';"    "});"    "};"    "refreshBtn.onclick = scan;"    "scan();"    "})();"    "</script>"    "</body>"    "</html>";

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;

    size_t n = 0;
    while (*p && *p != '"') {
        char value = *p++;
        if (value == '\\') {
            if (!*p) return false;
            value = *p++;
            if (value == 'n') value = '\n';
            else if (value == 'r') value = '\r';
            else if (value == 't') value = '\t';
        }
        if (n + 1 < out_size) out[n++] = value;
    }
    if (*p != '"') return false;
    out[n] = '\0';
    return true;
}

static void json_append_string(char *dst, size_t capacity, size_t *used, const char *src)
{
    if (!dst || !used || !src || *used >= capacity) return;
    while (*src && *used + 1 < capacity) {
        char c = *src++;
        if ((c == '"' || c == '\\') && *used + 2 < capacity) {
            dst[(*used)++] = '\\';
        }
        dst[(*used)++] = c;
    }
    dst[*used] = '\0';
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_PROVISION_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(nvs, "ssid", ssid);
    if (ret == ESP_OK) ret = nvs_set_str(nvs, "password", password);
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

esp_err_t wifi_provision_load_sta_config(wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_PROVISION_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    size_t ssid_size = sizeof(config->sta.ssid);
    ret = nvs_get_str(nvs, "ssid", (char *)config->sta.ssid, &ssid_size);
    if (ret == ESP_OK) {
        size_t password_size = sizeof(config->sta.password);
        esp_err_t password_ret = nvs_get_str(nvs, "password", (char *)config->sta.password,
                                              &password_size);
        if (password_ret != ESP_OK && password_ret != ESP_ERR_NVS_NOT_FOUND) ret = password_ret;
    }
    nvs_close(nvs);
    return ret;
}

static void http_stop(void);
static void dns_stop(void);
static void finish_provisioning_session(void);
static esp_err_t start_sta_connect(const char *ssid, const char *password);

static bool sta_is_associated(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static esp_err_t disconnect_current_sta(void)
{
    if (!sta_is_associated()) return ESP_OK;

    ESP_LOGI(TAG, "Disconnecting current STA before joining '%s'", s_target_ssid);
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_ERR_WIFI_NOT_CONNECT) return ESP_OK;
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < 50 && sta_is_associated(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* Drain the expected disconnect event before starting a new join. */
    vTaskDelay(pdMS_TO_TICKS(100));
    return sta_is_associated() ? ESP_ERR_TIMEOUT : ESP_OK;
}

static void restore_previous_sta(void)
{
    wifi_config_t old_config = {0};
    if (wifi_provision_load_sta_config(&old_config) != ESP_OK || old_config.sta.ssid[0] == '\0') {
        return;
    }
    ESP_LOGW(TAG, "Restoring previous STA '%s'", (char *)old_config.sta.ssid);
    if (esp_wifi_set_config(WIFI_IF_STA, &old_config) == ESP_OK) {
        (void)esp_wifi_connect();
    }
}

static void sta_connect_task(void *arg)
{
    (void)arg;
    /* Let the HTTP response reach the phone before tearing down SoftAP. */
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, s_target_ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, s_target_password, sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = s_target_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    dns_stop();
    http_stop();

    /* STA-only avoids SoftAP CSA + NVS write while flash cache is disabled. */
    s_expected_disconnect = true;
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) ret = disconnect_current_sta();
    s_expected_disconnect = false;
    if (ret == ESP_OK) ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Connecting STA to '%s'", s_target_ssid);
        ret = esp_wifi_connect();
    }
    if (ret != ESP_OK) {
        s_status = WIFI_PROVISION_STATUS_FAILED;
        ESP_LOGE(TAG, "Provisioning STA connect failed: %s", esp_err_to_name(ret));
    } else {
        for (int i = 0; i < 200 && s_status == WIFI_PROVISION_STATUS_CONNECTING; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_status == WIFI_PROVISION_STATUS_CONNECTING) {
            s_status = WIFI_PROVISION_STATUS_FAILED;
            ESP_LOGW(TAG, "Provisioning STA connection timed out");
        }
    }

    if (s_status != WIFI_PROVISION_STATUS_SUCCESS) {
        restore_previous_sta();
    }
    finish_provisioning_session();
    s_connect_task = NULL;
    wifi_provision_delete_current_task();
}

static esp_err_t http_send_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_wifi_html, sizeof(s_wifi_html) - 1);
}

static esp_err_t http_handle_scan(httpd_req_t *req)
{
    /* 扫描双保险：esp_wifi_scan_start() 与 STA 连接互斥，驱动在
     * WIFI_STATE_CONNECTING 下会直接报 "sta is connecting, return error"，
     * 表现为配网页永远 No networks found。
     *
     * 这里必须「无条件」调 esp_wifi_disconnect()，不能先判断 sta_is_associated()：
     * 设备可能「未关联但停在 CONNECTING」（未配网时反复重连的典型态），
     * 此时 esp_wifi_sta_get_ap_info() 失败 → 旧逻辑会跳过断开 → 扫描仍被拒。
     * esp_wifi_disconnect() 能把 CONNECTING / ASSOCIATED 统一打回 IDLE。
     * ESP_ERR_WIFI_NOT_CONNECT 表示本来就没在连，属正常，继续扫描。 */
    bool restore_sta = (s_status != WIFI_PROVISION_STATUS_CONNECTING);
    esp_err_t dret = esp_wifi_disconnect();
    if (dret != ESP_OK && dret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Pre-scan disconnect failed: %s", esp_err_to_name(dret));
    }
    /* 给驱动少量时间落回 IDLE（扫描阻塞式调用，无法用事件等待） */
    vTaskDelay(pdMS_TO_TICKS(50));

    wifi_scan_config_t scan_config = {
        .show_hidden = true,   /* 隐藏 SSID 的 AP 也要能扫到 */
    };
    uint16_t count = 0;
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret == ESP_OK) ret = esp_wifi_scan_get_ap_num(&count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(ret));
    }

    /* 扫描完成，恢复 STA 连接（若已在配网连接流程中则不动，
     * 由配网自己的状态机接管，避免打断用户手动连接） */
    if (restore_sta && !wifi_provision_is_running()) {
        esp_wifi_connect();
    }

    if (ret != ESP_OK || count == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    ret = esp_wifi_scan_get_ap_records(&count, records);
    if (ret != ESP_OK) {
        free(records);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan read failed");
    }

    size_t capacity = 2048;
    char *json = calloc(1, capacity);
    if (!json) {
        free(records);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t used = 0;
    json[used++] = '[';
    for (uint16_t i = 0; i < count && used + 80 < capacity; i++) {
        if (i) json[used++] = ',';
        int written = snprintf(json + used, capacity - used, "{\"ssid\":\"");
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
        json_append_string(json, capacity, &used, (const char *)records[i].ssid);
        written = snprintf(json + used, capacity - used, "\",\"rssi\":%d}", records[i].rssi);
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    json[used++] = ']';
    json[used] = '\0';

    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, json);
    free(json);
    free(records);
    return ret;
}

static esp_err_t http_handle_connect(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    }

    char body[256];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body read failed");
        received += (size_t)n;
    }
    body[received] = '\0';

    char ssid[sizeof(s_target_ssid)] = {0};
    char password[sizeof(s_target_password)] = {0};
    if (!json_get_string(body, "\"ssid\"", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
    }
    if (!json_get_string(body, "\"password\"", password, sizeof(password))) {
        password[0] = '\0';
    }

    esp_err_t ret = start_sta_connect(ssid, password);
    if (ret == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connect already in progress");
    }
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connect task failed");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"success\":true}");
}

static esp_err_t http_handle_status(httpd_req_t *req)
{
    const char *status = "idle";
    switch (s_status) {
    case WIFI_PROVISION_STATUS_CONNECTING: status = "connecting"; break;
    case WIFI_PROVISION_STATUS_SUCCESS: status = "success"; break;
    case WIFI_PROVISION_STATUS_FAILED: status = "failed"; break;
    default: break;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"status\":\"%s\"}", status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t http_handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_PROVISION_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    config.stack_size = WIFI_PROVISION_HTTP_STACK_BYTES;
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.lru_purge_enable = true;
    /* Captive portal stays on :80. ctrl_port must not collide with DLNA httpd. */
    config.server_port = 80;
    config.ctrl_port = 32780;
    esp_err_t ret = httpd_start(&s_httpd, &config);
    if (ret != ESP_OK) return ret;

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_send_root,
    };
    const httpd_uri_t scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = http_handle_scan,
    };
    const httpd_uri_t connect = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = http_handle_connect,
    };
    const httpd_uri_t status = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = http_handle_status,
    };
    const httpd_uri_t redirect = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_handle_redirect,
    };
    if (httpd_register_uri_handler(s_httpd, &root) != ESP_OK ||
        httpd_register_uri_handler(s_httpd, &scan) != ESP_OK ||
        httpd_register_uri_handler(s_httpd, &connect) != ESP_OK ||
        httpd_register_uri_handler(s_httpd, &status) != ESP_OK ||
        httpd_register_uri_handler(s_httpd, &redirect) != ESP_OK) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void http_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_sock = -1;
        wifi_provision_delete_current_task();
        return;
    }
    s_dns_sock = sock;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(WIFI_PROVISION_DNS_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        lwip_close(sock);
        s_dns_sock = -1;
        wifi_provision_delete_current_task();
        return;
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t packet[512];
    while (s_running) {
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        int length = recvfrom(sock, packet, sizeof(packet), 0,
                              (struct sockaddr *)&source, &source_len);
        if (length < (int)sizeof(dns_header_t)) continue;
        if (length > (int)sizeof(packet) - 16) continue;

        dns_header_t *header = (dns_header_t *)packet;
        uint8_t *cursor = packet + sizeof(dns_header_t);
        uint8_t *end = packet + length;
        while (cursor < end && *cursor) {
            if (*cursor > 63 || cursor + *cursor + 1 >= end) {
                cursor = end;
                break;
            }
            cursor += *cursor + 1;
        }
        if (cursor + 5 > end) continue;

        uint8_t *answer = packet + length;
        answer[0] = 0xC0; answer[1] = 0x0C;
        answer[2] = 0x00; answer[3] = 0x01;
        answer[4] = 0x00; answer[5] = 0x01;
        answer[6] = 0x00; answer[7] = 0x00; answer[8] = 0x00; answer[9] = 0x1E;
        answer[10] = 0x00; answer[11] = 0x04;
        answer[12] = 192; answer[13] = 168; answer[14] = 4; answer[15] = 1;
        header->flags = htons(0x8180);
        header->ancount = htons(1);
        sendto(sock, packet, length + 16, 0, (struct sockaddr *)&source, source_len);
    }

    lwip_close(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    wifi_provision_delete_current_task();
}

static esp_err_t dns_start(void)
{
    if (s_dns_task) return ESP_OK;
    return wifi_provision_create_task(dns_task, "wifi_dns",
                                      WIFI_PROVISION_DNS_STACK_BYTES,
                                      NULL, 3, &s_dns_task) == pdPASS ? ESP_OK : ESP_FAIL;
}

static void dns_stop(void)
{
    int sock = s_dns_sock;
    s_dns_sock = -1;
    if (sock >= 0) lwip_close(sock);
    for (int i = 0; i < 20 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_dns_task = NULL;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (!s_running || base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_DISCONNECTED && s_status == WIFI_PROVISION_STATUS_CONNECTING) {
        if (s_expected_disconnect) {
            s_expected_disconnect = false;
            return;
        }
        s_status = WIFI_PROVISION_STATUS_FAILED;
        ESP_LOGW(TAG, "Provisioning STA connection failed");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (!s_running || base != IP_EVENT || id != IP_EVENT_STA_GOT_IP || s_status != WIFI_PROVISION_STATUS_CONNECTING) {
        return;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (strncmp((const char *)ap.ssid, s_target_ssid, sizeof(s_target_ssid)) != 0) return;

    if (save_credentials(s_target_ssid, s_target_password) == ESP_OK) {
        s_status = WIFI_PROVISION_STATUS_SUCCESS;
        ESP_LOGI(TAG, "Provisioning succeeded, SSID '%s'", s_target_ssid);
    } else {
        s_status = WIFI_PROVISION_STATUS_FAILED;
        ESP_LOGE(TAG, "Saving provisioned credentials failed");
    }
}

static void finish_provisioning_session(void)
{
    s_running = false;
    http_stop();
    dns_stop();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);
    s_expected_disconnect = false;
}

esp_err_t wifi_provision_start(void)
{
    if (s_running) return ESP_OK;

    esp_err_t ret = esp_wifi_get_mode(&s_previous_mode);
    if (ret != ESP_OK) return ret;

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "Read SoftAP MAC failed");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ESP-Brookesia-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Set APSTA mode failed");
    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) goto fail_mode;

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (ret != ESP_OK) goto fail_mode;
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);
    if (ret != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        goto fail_mode;
    }

    s_status = WIFI_PROVISION_STATUS_IDLE;
    s_target_ssid[0] = '\0';
    s_target_password[0] = '\0';
    s_expected_disconnect = false;
    s_running = true;
    ret = http_start();
    if (ret == ESP_OK) ret = dns_start();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Provisioning started, AP SSID: %s", s_ap_ssid);
        return ESP_OK;
    }

    s_running = false;
    dns_stop();
    http_stop();
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);

fail_mode:
    esp_wifi_set_mode(s_previous_mode);
    return ret;
}

esp_err_t wifi_provision_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    http_stop();
    dns_stop();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);
    s_target_ssid[0] = '\0';
    s_target_password[0] = '\0';
    s_expected_disconnect = false;
    s_status = WIFI_PROVISION_STATUS_IDLE;
    return esp_wifi_set_mode(s_previous_mode);
}

bool wifi_provision_is_running(void)
{
    return s_running;
}

const char *wifi_provision_get_ap_ssid(void)
{
    return s_running ? s_ap_ssid : NULL;
}

wifi_provision_status_t wifi_provision_get_status(void)
{
    return s_status;
}

const char *wifi_provision_get_target_ssid(void)
{
    return s_target_ssid[0] ? s_target_ssid : NULL;
}

static esp_err_t ensure_connect_session(void)
{
    if (s_running) return ESP_OK;

    esp_err_t ret = esp_wifi_get_mode(&s_previous_mode);
    if (ret != ESP_OK) return ret;

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (ret != ESP_OK) return ret;
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);
    if (ret != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        return ret;
    }

    s_expected_disconnect = false;
    s_running = true;
    return ESP_OK;
}

static esp_err_t start_sta_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_connect_task) return ESP_ERR_INVALID_STATE;

    bool started_session = !s_running;
    esp_err_t ret = ensure_connect_session();
    if (ret != ESP_OK) return ret;

    memset(s_target_ssid, 0, sizeof(s_target_ssid));
    memset(s_target_password, 0, sizeof(s_target_password));
    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    if (password) {
        strncpy(s_target_password, password, sizeof(s_target_password) - 1);
    }

    s_status = WIFI_PROVISION_STATUS_CONNECTING;
    BaseType_t ok = xTaskCreatePinnedToCore(sta_connect_task, "wifi_sta_connect",
                                            4096, NULL, 5, &s_connect_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_connect_task = NULL;
        s_status = WIFI_PROVISION_STATUS_FAILED;
        if (started_session) finish_provisioning_session();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_provision_connect(const char *ssid, const char *password)
{
    return start_sta_connect(ssid, password);
}
