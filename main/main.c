#include "nvs_flash.h"
#include "esp_log.h"
#include "announcer.h"

static const char *TAG = "main";

void app_main(void)
{
    // NVS must be initialised before wifi_connect — the WiFi driver reads from NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition stale — erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(announcer_init());
}
