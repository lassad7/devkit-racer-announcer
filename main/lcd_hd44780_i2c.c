#include "lcd_hd44780_i2c.h"
#include "announcer_config.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "lcd";

// ---------------------------------------------------------------------------
// PCF8574 bit mapping (standard wiring used by most I2C LCD backpacks)
// ---------------------------------------------------------------------------
#define PCF_RS  0x01    // Register select: 0 = command, 1 = data
#define PCF_RW  0x02    // Read/write: always keep 0 (write)
#define PCF_EN  0x04    // Enable: pulse high→low to latch nibble
#define PCF_BL  0x08    // Backlight: 1 = on
// Bits 7-4 carry D7-D4 (the 4-bit data nibble)

// ---------------------------------------------------------------------------
// HD44780 command bytes
// ---------------------------------------------------------------------------
#define HD_CLEAR        0x01    // Clear display (needs 2 ms settle)
#define HD_HOME         0x02    // Return cursor home (needs 2 ms settle)
#define HD_ENTRY_MODE   0x06    // Increment cursor, no display shift
#define HD_DISPLAY_ON   0x0C    // Display on, cursor off, blink off
#define HD_4BIT_2LINE   0x28    // 4-bit bus, 2 lines, 5×8 font
#define HD_ROW0_BASE    0x00    // DDRAM address for row 0, col 0
#define HD_ROW1_BASE    0x40    // DDRAM address for row 1, col 0

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_lcd_dev;

// ---------------------------------------------------------------------------
// Low-level PCF8574 write
// ---------------------------------------------------------------------------

static void pcf8574_write(uint8_t data)
{
    // Backlight bit is always set so the display stays lit
    uint8_t byte = data | PCF_BL;
    i2c_master_transmit(s_lcd_dev, &byte, 1, pdMS_TO_TICKS(10));
}

// Pulse EN high then low to latch the nibble already on D7-D4
static void lcd_pulse_enable(uint8_t data)
{
    pcf8574_write(data | PCF_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
    pcf8574_write(data & ~PCF_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
}

// Send the upper nibble of `nibble` to D7-D4, with RS set for data/command
static void lcd_write_nibble(uint8_t nibble, bool is_data)
{
    uint8_t pcf_byte = nibble & 0xF0;
    if (is_data) pcf_byte |= PCF_RS;
    lcd_pulse_enable(pcf_byte);
}

// Send a full byte to the HD44780 as two 4-bit nibbles (high nibble first)
static void lcd_write_byte(uint8_t byte, bool is_data)
{
    lcd_write_nibble(byte & 0xF0,        is_data);   // high nibble
    lcd_write_nibble((byte << 4) & 0xF0, is_data);   // low nibble
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void lcd_clear(void)
{
    lcd_write_byte(HD_CLEAR, false);
    vTaskDelay(pdMS_TO_TICKS(2));   // Clear takes up to 1.52 ms
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    uint8_t addr = (row == 0 ? HD_ROW0_BASE : HD_ROW1_BASE) + col;
    lcd_write_byte(0x80 | addr, false);
}

void lcd_print(const char *str)
{
    while (*str) {
        lcd_write_byte((uint8_t)*str++, true);
    }
}

void lcd_print_line(uint8_t row, const char *str)
{
    // Write exactly LCD_COLS characters: left-align str, pad remainder with spaces.
    // snprintf truncates if str is longer, ensuring we never write past column 15.
    char buf[LCD_COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, str);
    lcd_set_cursor(0, row);
    lcd_print(buf);
}

esp_err_t lcd_init(void)
{
    // --- I2C master bus ---
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                   = LCD_I2C_PORT,
        .sda_io_num                 = LCD_SDA_PIN,
        .scl_io_num                 = LCD_SCL_PIN,
        .clk_source                 = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt          = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // --- PCF8574 device ---
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LCD_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_lcd_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    // --- HD44780 4-bit initialisation sequence ---
    // Per datasheet: send three 0x3 nibbles then switch to 4-bit.
    vTaskDelay(pdMS_TO_TICKS(50));          // >40 ms power-on delay

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(5));           // >4.1 ms

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));           // >100 µs

    lcd_write_nibble(0x30, false);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_write_nibble(0x20, false);          // Switch to 4-bit mode
    vTaskDelay(pdMS_TO_TICKS(1));

    // Now in 4-bit mode — send full-byte configuration commands
    lcd_write_byte(HD_4BIT_2LINE,  false);  // 4-bit, 2 lines, 5×8 font
    lcd_write_byte(HD_DISPLAY_ON,  false);  // Display on, cursor off
    lcd_write_byte(HD_CLEAR,       false);  // Clear display
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_write_byte(HD_ENTRY_MODE,  false);  // Cursor increments left→right

    ESP_LOGI(TAG, "HD44780 ready (I2C addr=0x%02X, SDA=%d, SCL=%d)",
             LCD_I2C_ADDR, LCD_SDA_PIN, LCD_SCL_PIN);
    return ESP_OK;
}
