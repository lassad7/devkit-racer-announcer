#pragma once

#include "esp_err.h"

/**
 * Initialise the active buzzer GPIO.
 * Must be called once before play_fanfare().
 */
esp_err_t fanfare_init(void);

/**
 * Play a short victory buzz pattern (ta-ta-taaaa, ~880 ms total).
 * Only called on entry to the FINISHED state.
 */
void play_fanfare(void);
