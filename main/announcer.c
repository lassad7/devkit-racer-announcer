#include "announcer.h"
#include "announcer_config.h"
#include "announcer_strings.h"
#include "firebase_client.h"
#include "firebase_config.h"
#include "lcd_hd44780_i2c.h"
#include "fanfare.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "announcer";

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef enum {
    APP_STATE_UNKNOWN,
    APP_STATE_IDLE,
    APP_STATE_VOTING,
    APP_STATE_RACING,
    APP_STATE_FINISHED,
} app_state_t;

// ---------------------------------------------------------------------------
// Helper: parse integer from a Firebase REST response.
// Firebase may return bare integers or JSON-quoted strings.
// ---------------------------------------------------------------------------
static int parse_firebase_int(const char *buf)
{
    const char *p = buf;
    while (*p == '"' || *p == ' ') p++;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Helper: copy the string value from a Firebase REST response into dst,
// stripping surrounding JSON double-quotes if present.
// ---------------------------------------------------------------------------
static void strip_firebase_quotes(const char *src, char *dst, size_t dst_len)
{
    const char *p = src;
    while (*p == '"' || *p == ' ') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '"' || p[len - 1] == ' ')) len--;
    size_t copy_len = (len < dst_len - 1) ? len : dst_len - 1;
    memcpy(dst, p, copy_len);
    dst[copy_len] = '\0';
}

// ---------------------------------------------------------------------------
// Display functions
// ---------------------------------------------------------------------------

static void lcd_show_idle(const race_result_t *last, bool first_entry)
{
    static int joke_idx   = 0;
    static int poll_count = 0;

    const int polls_per_joke = JOKE_INTERVAL_MS / STATE_POLL_INTERVAL_MS;

    if (first_entry) {
        lcd_clear();
        poll_count = 0;
    }

    poll_count++;
    if (poll_count >= polls_per_joke) {
        joke_idx  = (joke_idx + 1) % JOKE_STRINGS_COUNT;
        poll_count = 0;
        lcd_print_line(1, JOKE_STRINGS[joke_idx]);
    }

    if (first_entry) {
        lcd_print_line(0, "Hello Racers!");
        lcd_print_line(1, JOKE_STRINGS[joke_idx]);
    }
}

static void lcd_show_voting(int v1, int v2, bool first_entry)
{
    char line0[LCD_COLS + 1];
    snprintf(line0, sizeof(line0), "Steve:%d  Bob:%d", v1, v2);
    if (first_entry) {
        lcd_clear();
        lcd_print_line(1, "  CAST YOUR VOTE");
    }
    lcd_print_line(0, line0);
}

static void lcd_show_racing(void)
{
    lcd_clear();
    lcd_print_line(0, "RACE IN PROGRESS");
    lcd_print_line(1, "   GO GO GO!    ");
}

// play_fanfare is now implemented in fanfare.c

static void lcd_show_finished(const race_result_t *result)
{
    char line0[32];
    char line1[32];

    const char *display_name = strstr(result->winner, "racer_1") ? "Steve" :
                               strstr(result->winner, "racer_2") ? "Bob" : result->winner;
    snprintf(line0, sizeof(line0), "Win: %s", display_name);
    snprintf(line1, sizeof(line1), "1:%.1fs  2:%.1fs",
             result->time_racer_1 / 1000.0f, result->time_racer_2 / 1000.0f);
    lcd_clear();

    lcd_print_line(0, line0);
    lcd_print_line(1, line1);

    play_fanfare();
}

// ---------------------------------------------------------------------------
// Main announcer task — polls Firebase state and dispatches to display funcs.
// ---------------------------------------------------------------------------

static void announcer_task(void *arg)
{
    char state_buf[32];
    app_state_t current_state = APP_STATE_UNKNOWN;
    race_result_t last_result = {
        .winner       = "none",
        .time_racer_1 = 0,
        .time_racer_2 = 0,
        .valid        = false,
    };

    for (;;) {
        if (firebase_get("/game/state", state_buf, sizeof(state_buf)) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read /game/state — will retry next poll");
            vTaskDelay(pdMS_TO_TICKS(STATE_POLL_INTERVAL_MS));
            continue;
        }

        // Determine new state from Firebase string value (JSON-quoted).
        app_state_t new_state;
        if      (strstr(state_buf, "idle"))     new_state = APP_STATE_IDLE;
        else if (strstr(state_buf, "voting"))   new_state = APP_STATE_VOTING;
        else if (strstr(state_buf, "racing"))   new_state = APP_STATE_RACING;
        else if (strstr(state_buf, "finished")) new_state = APP_STATE_FINISHED;
        else                                    new_state = APP_STATE_UNKNOWN;

        bool state_changed = (new_state != current_state);
        if (state_changed) {
            current_state = new_state;
            char clean[32];
            strip_firebase_quotes(state_buf, clean, sizeof(clean));
            ESP_LOGI(TAG, "State transition -> %s", clean);
        }

        switch (current_state) {

        case APP_STATE_IDLE:
            lcd_show_idle(&last_result, state_changed);
            break;

        case APP_STATE_VOTING: {
            char v1_buf[16], v2_buf[16];
            int v1 = 0, v2 = 0;
            if (firebase_get("/game/votes/racer_1", v1_buf, sizeof(v1_buf)) == ESP_OK) {
                v1 = parse_firebase_int(v1_buf);
            }
            if (firebase_get("/game/votes/racer_2", v2_buf, sizeof(v2_buf)) == ESP_OK) {
                v2 = parse_firebase_int(v2_buf);
            }
            lcd_show_voting(v1, v2, state_changed);
            break;
        }

        case APP_STATE_RACING:
            // Only redraw on transition — the message is static.
            if (state_changed) {
                lcd_show_racing();
            }
            break;

        case APP_STATE_FINISHED:
            if (state_changed) {
                char winner_buf[32], t1_buf[16], t2_buf[16];

                if (firebase_get("/game/race/winner", winner_buf, sizeof(winner_buf)) == ESP_OK) {
                    strip_firebase_quotes(winner_buf, last_result.winner,
                                         sizeof(last_result.winner));
                } else {
                    strlcpy(last_result.winner, "unknown", sizeof(last_result.winner));
                }
                if (firebase_get("/game/race/time_racer_1", t1_buf, sizeof(t1_buf)) == ESP_OK) {
                    last_result.time_racer_1 = parse_firebase_int(t1_buf);
                }
                if (firebase_get("/game/race/time_racer_2", t2_buf, sizeof(t2_buf)) == ESP_OK) {
                    last_result.time_racer_2 = parse_firebase_int(t2_buf);
                }
                last_result.valid = true;

                lcd_show_finished(&last_result);
                vTaskDelay(pdMS_TO_TICKS(RESULTS_HOLD_MS));
            }
            break;

        case APP_STATE_UNKNOWN:
            ESP_LOGW(TAG, "Unrecognised state value: %s", state_buf);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(STATE_POLL_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

esp_err_t announcer_init(void)
{
    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(wifi_connect());

    ESP_LOGI(TAG, "Initialising Firebase...");
    ESP_ERROR_CHECK(firebase_init(FIREBASE_PROJECT_URL, FIREBASE_API_KEY));

    ESP_LOGI(TAG, "Initialising LCD...");
    ESP_ERROR_CHECK(lcd_init());

    ESP_LOGI(TAG, "Initialising fanfare...");
    ESP_ERROR_CHECK(fanfare_init());

    xTaskCreate(announcer_task, "announcer_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Announcer task started");

    return ESP_OK;
}
