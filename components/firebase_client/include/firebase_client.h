#pragma once
#include "esp_err.h"
#include <stddef.h>

/**
 * Initialise the Firebase client. Call once after WiFi is connected.
 * @param project_url  Full database URL, e.g. "https://project-default-rtdb.firebaseio.com"
 * @param api_key      Firebase Web API key (from Project Settings → General)
 */
esp_err_t firebase_init(const char *project_url, const char *api_key);

/**
 * HTTP GET at path. Writes raw JSON response into out_buf (null-terminated).
 * Returns ESP_OK on HTTP 200, ESP_FAIL otherwise.
 */
esp_err_t firebase_get(const char *path, char *out_buf, size_t buf_len);

/**
 * HTTP PUT at path. Replaces the node with value_json.
 * value_json must be valid JSON (e.g. "\"idle\"" for a string, "42" for an integer).
 */
esp_err_t firebase_put(const char *path, const char *value_json);

/**
 * HTTP PATCH at path. Merges patch_json fields into the existing node.
 * Use for multi-field updates to avoid separate round trips.
 */
esp_err_t firebase_patch(const char *path, const char *patch_json);

/**
 * Read integer at path, add delta, write back. Low-contention only.
 * Not atomic — suitable for vote counting at low concurrency.
 */
esp_err_t firebase_increment(const char *path, int delta);

/**
 * Connect to WiFi using credentials from firebase_config.h.
 * Blocks until connected or WIFI_RETRY_MAX retries exceeded.
 * Logs IP address on success. Returns ESP_FAIL if connection not established.
 *
 * Prerequisites (call once in app_main before wifi_connect):
 *   nvs_flash_init()  — required by the WiFi driver
 *
 * Safe to call even if esp_event_loop_create_default() was already called
 * by the application — the existing event loop is reused.
 */
esp_err_t wifi_connect(void);
