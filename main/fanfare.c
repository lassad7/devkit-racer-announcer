#include "fanfare.h"
#include "announcer_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fanfare";

// Active LOW buzzer: GPIO LOW = buzzing, GPIO HIGH = silent.
#define BUZZER_ON  0
#define BUZZER_OFF 1

static void beep(int on_ms, int off_ms)
{
    gpio_set_level(BUZZER_PIN, BUZZER_ON);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    gpio_set_level(BUZZER_PIN, BUZZER_OFF);
    if (off_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

esp_err_t fanfare_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask  = (1ULL << BUZZER_PIN),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(BUZZER_PIN, BUZZER_OFF);  // start silent
    ESP_LOGI(TAG, "Buzzer ready on GPIO%d", BUZZER_PIN);
    return ESP_OK;
}

void play_fanfare(void)
{
    ESP_LOGI(TAG, "play_fanfare: victory beeps");
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 10; i++) {
            beep(60, 60);
        }
        vTaskDelay(pdMS_TO_TICKS(300));  // pause between rounds
    }
    ESP_LOGI(TAG, "play_fanfare: done");
}
