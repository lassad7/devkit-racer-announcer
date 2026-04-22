#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Initialise the HD44780 LCD over I2C via a PCF8574 backpack.
 * Pin and address configuration comes from announcer_config.h.
 * Must be called before any other lcd_* function.
 */
esp_err_t lcd_init(void);

/** Clear all characters and return cursor to home position. */
void lcd_clear(void);

/** Move cursor to column col (0-based), row row (0 or 1). */
void lcd_set_cursor(uint8_t col, uint8_t row);

/** Write a string starting at the current cursor position. */
void lcd_print(const char *str);

/**
 * Write exactly 16 characters to the given row (0 or 1).
 * str is left-aligned and padded with spaces to fill the row,
 * preventing stale characters from previous content.
 */
void lcd_print_line(uint8_t row, const char *str);
