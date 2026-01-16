/*
 * WiFi Manager for ESP32-Luna
 * Handles WiFi connection with NVS-stored credentials and auto-reconnect
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NAMESPACE "luna_wifi"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASS_KEY "password"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_initialized = false;
static bool s_connected = false;
static int s_retry_count = 0;
static const int MAX_RETRY = 10;

static wifi_manager_event_cb_t s_event_callback = NULL;
static void *s_event_callback_ctx = NULL;

static esp_ip4_addr_t s_ip_addr;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_connected = false;
                if (s_retry_count < MAX_RETRY) {
                    esp_wifi_connect();
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry connect to AP (%d/%d)", s_retry_count, MAX_RETRY);
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    ESP_LOGE(TAG, "Failed to connect after %d retries", MAX_RETRY);
                }
                if (s_event_callback) {
                    s_event_callback(WIFI_EVENT_DISCONNECTED, s_event_callback_ctx);
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to AP");
                s_retry_count = 0;
                if (s_event_callback) {
                    s_event_callback(WIFI_EVENT_CONNECTED, s_event_callback_ctx);
                }
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                s_ip_addr = event->ip_info.ip;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&s_ip_addr));
                s_connected = true;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                if (s_event_callback) {
                    s_event_callback(WIFI_EVENT_GOT_IP, s_event_callback_ctx);
                }
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP address");
                s_connected = false;
                if (s_event_callback) {
                    s_event_callback(WIFI_EVENT_LOST_IP, s_event_callback_ctx);
                }
                break;

            default:
                break;
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP,
        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");

    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_initialized = false;
    s_connected = false;

    return ESP_OK;
}

esp_err_t wifi_manager_connect(const wifi_manager_config_t *config)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear event group bits and reset retry count for fresh start
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    // Store in NVS if requested
    if (config->store_in_nvs) {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            nvs_set_str(nvs, WIFI_NVS_SSID_KEY, config->ssid);
            nvs_set_str(nvs, WIFI_NVS_PASS_KEY, config->password);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Credentials stored in NVS");
        }
    }

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password) - 1);

    // Set auth mode based on password - allow open networks if no password
    if (strlen(config->password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "Connecting to open network (no password)");
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", config->ssid);

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_connect_stored(void)
{
    wifi_manager_config_t config = {0};

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No stored credentials found");
        return ESP_ERR_NOT_FOUND;
    }

    size_t ssid_len = sizeof(config.ssid);
    size_t pass_len = sizeof(config.password);

    err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, config.ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_get_str(nvs, WIFI_NVS_PASS_KEY, config.password, &pass_len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    config.store_in_nvs = false;
    return wifi_manager_connect(&config);
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_disconnect();
    s_connected = false;

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_manager_get_ip(char *ip_str)
{
    if (!s_connected || ip_str == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sprintf(ip_str, IPSTR, IP2STR(&s_ip_addr));
    return ESP_OK;
}

void wifi_manager_set_event_callback(wifi_manager_event_cb_t callback, void *ctx)
{
    s_event_callback = callback;
    s_event_callback_ctx = ctx;
}

esp_err_t wifi_manager_clear_stored(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Stored credentials cleared");
    return ESP_OK;
}
