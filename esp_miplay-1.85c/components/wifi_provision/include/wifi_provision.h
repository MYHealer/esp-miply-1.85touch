#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_PROVISION_STATUS_IDLE = 0,
    WIFI_PROVISION_STATUS_CONNECTING,
    WIFI_PROVISION_STATUS_SUCCESS,
    WIFI_PROVISION_STATUS_FAILED,
} wifi_provision_status_t;

/* Load credentials saved by the provisioning portal into a Wi-Fi config. */
esp_err_t wifi_provision_load_sta_config(wifi_config_t *config);

/* Start or stop the reference project's AP + captive portal provisioning flow. */
esp_err_t wifi_provision_start(void);
esp_err_t wifi_provision_stop(void);

bool wifi_provision_is_running(void);
const char *wifi_provision_get_ap_ssid(void);
const char *wifi_provision_get_target_ssid(void);
wifi_provision_status_t wifi_provision_get_status(void);

/* Connect STA using SSID + password from the device UI or captive portal. */
esp_err_t wifi_provision_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
