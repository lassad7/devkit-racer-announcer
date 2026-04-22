#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Cached result from the most recently completed race.
 * Populated on entry to the FINISHED state; displayed during IDLE.
 */
typedef struct {
    char winner[16];    // "racer_1" or "racer_2"
    int  time_racer_1;  // elapsed ms
    int  time_racer_2;  // elapsed ms
    bool valid;         // false until the first race completes
} race_result_t;

/**
 * Initialise the announcer: connects WiFi, initialises Firebase,
 * initialises peripherals, and launches the announcer polling task.
 * Call once from app_main after nvs_flash_init().
 */
esp_err_t announcer_init(void);
