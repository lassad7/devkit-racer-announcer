#pragma once

// ---------------------------------------------------------------------------
// LCD — HD44780 16x2 over I2C (PCF8574 backpack)
// ---------------------------------------------------------------------------
#define LCD_SDA_PIN      6
#define LCD_SCL_PIN      7
#define LCD_I2C_ADDR     0x27   // common PCF8574 address; try 0x3F if display blank
#define LCD_I2C_PORT     I2C_NUM_0
#define LCD_COLS         16
#define LCD_ROWS         2

// ---------------------------------------------------------------------------
// Active buzzer — GPIO output, HIGH = buzzing
// ---------------------------------------------------------------------------
#define BUZZER_PIN  5

// ---------------------------------------------------------------------------
// Timing constants (milliseconds)
// ---------------------------------------------------------------------------

// How often the main loop polls /game/state (and vote counts in VOTING state)
#define STATE_POLL_INTERVAL_MS   500

// How long to hold the results screen before returning to idle polling
#define RESULTS_HOLD_MS          5000

// How often the joke cycles in idle state
#define JOKE_INTERVAL_MS         2000
