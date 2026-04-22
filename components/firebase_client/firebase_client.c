#include "firebase_client.h"
#include "firebase_config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "firebase_client";

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_RETRY_MAX      5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_RETRY_MAX) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_retry_count, WIFI_RETRY_MAX);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    // esp_event_loop_create_default returns ESP_ERR_INVALID_STATE if the
    // caller's firmware already created the default event loop — treat as OK.
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_RETRY_MAX);
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Firebase init
// ---------------------------------------------------------------------------

static char s_project_url[128];
static char s_api_key[64];

esp_err_t firebase_init(const char *project_url, const char *api_key)
{
    strlcpy(s_project_url, project_url, sizeof(s_project_url));
    strlcpy(s_api_key,     api_key,     sizeof(s_api_key));
    ESP_LOGI(TAG, "Firebase client initialised for %s", s_project_url);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

#define FIREBASE_RETRY_MAX      3
#define FIREBASE_RETRY_DELAY_MS 500
#define URL_BUF_SIZE            256

static void build_url(char *buf, size_t len, const char *path)
{
    snprintf(buf, len, "%s%s.json?auth=%s", s_project_url, path, s_api_key);
}

// Parse an integer from a Firebase REST response, which may be a bare integer
// (42) or a JSON-quoted string ("42"). Scans past any leading quote or space.
static int parse_firebase_int(const char *buf)
{
    const char *p = buf;
    while (*p == '"' || *p == ' ') {
        p++;
    }
    return atoi(p);
}

// ---------------------------------------------------------------------------
// firebase_get
// ---------------------------------------------------------------------------

esp_err_t firebase_get(const char *path, char *out_buf, size_t buf_len)
{
    char url[URL_BUF_SIZE];
    build_url(url, sizeof(url), path);

    for (int attempt = 1; attempt <= FIREBASE_RETRY_MAX; attempt++) {
        esp_http_client_config_t config = {
            .url               = url,
            .method            = HTTP_METHOD_GET,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 5000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GET open failed at %s (attempt %d/%d): %s",
                     path, attempt, FIREBASE_RETRY_MAX, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(FIREBASE_RETRY_DELAY_MS));
            continue;
        }

        int64_t content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status != 200) {
            ESP_LOGW(TAG, "GET HTTP %d at %s (attempt %d/%d)",
                     status, path, attempt, FIREBASE_RETRY_MAX);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(FIREBASE_RETRY_DELAY_MS));
            continue;
        }

        // content_len is -1 for chunked transfers; treat as potentially fitting
        if (content_len >= (int64_t)buf_len) {
            ESP_LOGW(TAG, "GET response too large (%lld bytes) for buf_len %zu at %s",
                     content_len, buf_len, path);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int read = esp_http_client_read_response(client, out_buf, (int)buf_len - 1);
        out_buf[read > 0 ? read : 0] = '\0';
        esp_http_client_cleanup(client);
        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// firebase_put
// ---------------------------------------------------------------------------

esp_err_t firebase_put(const char *path, const char *value_json)
{
    char url[URL_BUF_SIZE];
    build_url(url, sizeof(url), path);

    for (int attempt = 1; attempt <= FIREBASE_RETRY_MAX; attempt++) {
        esp_http_client_config_t config = {
            .url               = url,
            .method            = HTTP_METHOD_PUT,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 5000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, value_json, (int)strlen(value_json));

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "PUT HTTP %d at %s (attempt %d/%d): %s",
                     status, path, attempt, FIREBASE_RETRY_MAX, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(FIREBASE_RETRY_DELAY_MS));
            continue;
        }

        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// firebase_patch
// ---------------------------------------------------------------------------

esp_err_t firebase_patch(const char *path, const char *patch_json)
{
    char url[URL_BUF_SIZE];
    build_url(url, sizeof(url), path);

    for (int attempt = 1; attempt <= FIREBASE_RETRY_MAX; attempt++) {
        esp_http_client_config_t config = {
            .url               = url,
            .method            = HTTP_METHOD_PATCH,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 5000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, patch_json, (int)strlen(patch_json));

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "PATCH HTTP %d at %s (attempt %d/%d): %s",
                     status, path, attempt, FIREBASE_RETRY_MAX, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(FIREBASE_RETRY_DELAY_MS));
            continue;
        }

        return ESP_OK;
    }

    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// firebase_increment
// ---------------------------------------------------------------------------

esp_err_t firebase_increment(const char *path, int delta)
{
    char buf[32];
    esp_err_t err = firebase_get(path, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    int current = parse_firebase_int(buf);
    char new_val[16];
    snprintf(new_val, sizeof(new_val), "%d", current + delta);
    return firebase_put(path, new_val);
}
