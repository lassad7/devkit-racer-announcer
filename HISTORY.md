# Announcer Firmware — Development History

This file logs what has been built, what was tested, and what remains.
A new session agent should read this before touching any code.

---

## Phase 1 — Infrastructure + State Machine (serial stubs)

**Status:** COMPLETE — verified on hardware 2026-04-15

### What was built

| File | Description |
|------|-------------|
| `partitions.csv` | Custom partition table (factory app = 3 MB, required for TLS bundle) |
| `sdkconfig.defaults` | Enables custom partition table and 4 MB flash |
| `main/CMakeLists.txt` | Registers `main.c`, `announcer.c`; links `firebase_client`, `nvs_flash` |
| `main/main.c` | `app_main`: NVS init → `announcer_init()` |
| `main/announcer_config.h` | All pin defines and timing constants |
| `main/announcer_strings.h` | `JOKE_STRINGS[]` array (edit before demo) |
| `main/announcer.h` | `race_result_t` struct + `announcer_init` declaration |
| `main/announcer.c` | Full state machine; display functions are **ESP_LOGI stubs** |

### State machine behaviour

- Polls `/game/state` every `STATE_POLL_INTERVAL_MS` (1000 ms).
- In `VOTING`: also reads `/game/votes/racer_1` and `/game/votes/racer_2` each poll.
- On transition to `FINISHED`: reads `/game/race/winner`, `time_racer_1`, `time_racer_2`; caches in `race_result_t`; holds screen for `RESULTS_HOLD_MS` (5 s).
- In `IDLE`: `lcd_show_idle` cycles through `JOKE_STRINGS[]` every `JOKE_INTERVAL_MS` (5 s) using an internal static counter.

### Phase 1 test checklist

Before moving to Phase 2, verify all of the following over serial monitor (`idf.py monitor`):

- [x] `idf.py build` succeeds with no errors or warnings
- [x] Serial: `WiFi connected, IP: x.x.x.x`
- [x] Serial: `Firebase client initialised for https://...`
- [x] Set `/game/state` → `"idle"` in Firebase console → see `[announcer] State transition -> idle` and idle log lines cycling every 5 s
- [x] Set `/game/state` → `"voting"` → see vote counts logged every 1 s
- [x] Set `/game/state` → `"racing"` → see racing log (once, on transition)
- [x] Set `/game/state` → `"finished"`, with `/game/race/winner`, `time_racer_1`, `time_racer_2` set → see winner/times logged, fanfare stub log, then 5 s pause before polling resumes

---

## Phase 2 — LCD Integration

**Status:** COMPLETE — verified on hardware 2026-04-15

### What needs to be built

- `main/lcd_hd44780_i2c.h` — HD44780 driver API
- `main/lcd_hd44780_i2c.c` — HD44780 via PCF8574 using ESP-IDF new I2C master driver (`driver/i2c_master.h`)
- `main/CMakeLists.txt` — add `lcd_hd44780_i2c.c` to SRCS; add `esp_driver_i2c` to PRIV_REQUIRES
- `main/announcer.c` — replace `ESP_LOGI` stubs in `lcd_show_*` with real LCD calls; call `lcd_init()` in `announcer_init`

### Hardware

- LCD: HD44780 16×2, PCF8574 I2C backpack
- SDA = GPIO6, SCL = GPIO7
- I2C address: `0x27` (try `0x3F` if display stays blank)

### LCD content layout

| State | Line 0 (16 chars) | Line 1 (16 chars) |
|-------|-------------------|-------------------|
| idle (no result) | `DevKit Racer!   ` | joke string |
| idle (with result) | `Win: racer_1    ` | joke string |
| voting | `R1: 000  R2: 000` | `  CAST YOUR VOTE` |
| racing | `RACE IN PROGRESS` | `   GO GO GO!    ` |
| finished | `Win: racer_1    ` | `T1:1234 T2:5678 ` |

### Phase 2 test checklist

- [x] Build + flash
- [x] LCD backlight on at boot
- [x] Idle: line 0 shows welcome/last winner; line 1 cycles jokes every 5 s
- [x] Voting: vote counts update every 1 s
- [x] Racing: static race-in-progress message
- [x] Finished: winner + times shown; after 5 s reverts to idle display

---

## Phase 3 — Buzzer Fanfare

**Status:** COMPLETE — verified on hardware 2026-04-16

### What needs to be built

- `main/fanfare.h` — `fanfare_init` + `play_fanfare` declarations
- `main/fanfare.c` — I2S tone generation using ESP-IDF new I2S driver (`driver/i2s_std.h`)
- `main/CMakeLists.txt` — add `fanfare.c` to SRCS; add `esp_driver_i2s` to PRIV_REQUIRES
- `main/announcer.c` — replace `play_fanfare` stub with real call; call `fanfare_init()` in `announcer_init`

### Hardware

- MAX98357A I2S amplifier
- BCLK = GPIO3, LRC = GPIO4, DIN = GPIO5, VIN = 5 V

### Fanfare design

- 4-note ascending sequence: C5 (523 Hz) → E5 (659 Hz) → G5 (784 Hz) → C6 (1047 Hz)
- 200 ms per note, square wave, 16-bit PCM, 16 kHz sample rate, stereo
- I2S TX disabled after fanfare to avoid DC noise on speaker

### Phase 3 test checklist

- [x] Build + flash
- [x] Set `/game/state` → `"finished"` → active buzzer plays victory pattern (5 × 10 beeps)
- [x] No sound during idle/voting/racing states
- [x] After `RESULTS_HOLD_MS`, display returns to idle polling

### Notes
- Switched from MAX98357A I2S amplifier to active buzzer on GPIO5 (active LOW)
- Victory pattern: 5 rounds of 10 short beeps (60 ms on / 60 ms off), 300 ms gap between rounds

---

## Key constraints (do not change without reading INTEGRATION.md)

- `nvs_flash_init()` **must** precede `wifi_connect()` — WiFi driver reads NVS at init
- Firebase GET returns strings JSON-quoted (`"idle"`, not `idle`) — always use `strstr`, never `strcmp`
- `announcer_task` stack = 4096 bytes minimum (HTTPS+TLS requirement)
- All Firebase response buffers must be stack-allocated locals, not statics
