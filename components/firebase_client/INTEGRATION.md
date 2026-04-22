# firebase_client — Integration Guide

**Target:** ESP32-C6 | **ESP-IDF:** v5.1+ (tested v5.5.2) | **Protocol:** HTTPS via Firebase REST API

This guide is the authoritative reference for integrating `firebase_client` into any DevKit Racer node firmware. It is structured for AI coding agents but written to be human-readable. Read it fully before writing integration code — several non-obvious behaviours are documented here that will cause silent bugs or hard aborts if missed.

---

## What This Component Does

`firebase_client` provides:
- WiFi station connection (`wifi_connect`) using credentials baked into `firebase_config.h` at compile time
- HTTPS reads and writes to Firebase Realtime Database via the REST API (`firebase_get`, `firebase_put`, `firebase_patch`, `firebase_increment`)
- TLS using ESP-IDF's built-in certificate bundle — no PEM files needed
- Automatic retry (3 attempts, 500 ms backoff) with `ESP_LOGW` on each failure

It does **not** provide: JSON parsing, task creation, or any game logic. Those belong in the node firmware.

---

## File Structure

```
components/firebase_client/
├── CMakeLists.txt
├── INTEGRATION.md              ← this file
├── include/
│   ├── firebase_client.h       ← public API — include this in your firmware
│   ├── firebase_config.h       ← credentials (gitignored, you must create this)
│   └── firebase_config.h.example
└── firebase_client.c
```

---

## Step-by-Step Integration

### 1. Copy the component

Drop the entire `firebase_client/` folder into the target project's `components/` directory:

```
your_node_project/
└── components/
    └── firebase_client/     ← paste here
```

ESP-IDF auto-discovers components in `components/` at the project root — no `CMakeLists.txt` changes at the project level are needed.

### 2. Create credentials file

```bash
cp components/firebase_client/include/firebase_config.h.example \
   components/firebase_client/include/firebase_config.h
```

Edit `firebase_config.h`:

```c
#define FIREBASE_PROJECT_URL  "https://your-project-default-rtdb.firebaseio.com"
#define FIREBASE_API_KEY      "your-web-api-key"
#define WIFI_SSID             "your-network-ssid"
#define WIFI_PASSWORD         "your-network-password"
```

Add to `.gitignore`:
```
components/firebase_client/include/firebase_config.h
```

### 3. Update `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES firebase_client nvs_flash
)
```

`nvs_flash` must be listed here because your `app_main` calls `nvs_flash_init()` directly. It is intentionally absent from the component's own `CMakeLists.txt` — the component does not call any NVS functions itself.

### 4. Partition table

The TLS certificate bundle is large. The default ESP-IDF factory partition (1 MB) is too small. Use a custom partition table. Copy these two files from the Starter project root into your project root if they are not already present:

**`partitions.csv`:**
```csv
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000,
phy_init, data, phy,      0xf000,   0x1000,
factory,  app,  factory,  0x10000,  0x300000,
```

**`sdkconfig.defaults`:**
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

After adding these, delete any existing `sdkconfig` and rebuild:
```bash
rm sdkconfig && idf.py build
```

### 5. Write the startup sequence in `app_main`

The startup sequence is strict. Deviation causes hard faults or silent failures.

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "firebase_client.h"
#include "firebase_config.h"

void app_main(void)
{
    // 1. NVS flash init — MUST come before wifi_connect. The WiFi driver
    //    uses NVS internally. Erase and reinit if the partition is stale.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. WiFi — blocks until IP acquired or WIFI_RETRY_MAX (5) attempts fail.
    ESP_ERROR_CHECK(wifi_connect());

    // 3. Firebase init — stores URL and API key; call once after WiFi is up.
    ESP_ERROR_CHECK(firebase_init(FIREBASE_PROJECT_URL, FIREBASE_API_KEY));

    // 4. Your node logic here (create tasks, read/write Firebase, etc.)
}
```

---

## API Reference

All functions return `esp_err_t`. Always check with `ESP_ERROR_CHECK()` or explicit handling — never ignore the return value.

---

### `wifi_connect`

```c
esp_err_t wifi_connect(void);
```

Reads `WIFI_SSID` and `WIFI_PASSWORD` from `firebase_config.h` at compile time. Internally calls `esp_netif_init()`, `esp_event_loop_create_default()`, `esp_wifi_init()`, and starts the WiFi driver. Blocks indefinitely until connected or retry limit exceeded.

**Prerequisites:** `nvs_flash_init()` must have been called first.

**Event loop:** If your firmware already called `esp_event_loop_create_default()`, `wifi_connect` handles this gracefully — it detects the existing loop and reuses it. Do not call `wifi_connect` twice.

**Returns:**
- `ESP_OK` — connected, IP acquired, IP logged via `ESP_LOGI`
- `ESP_FAIL` — all 5 retries exhausted; reason logged via `ESP_LOGE`

---

### `firebase_init`

```c
esp_err_t firebase_init(const char *project_url, const char *api_key);
```

Copies `project_url` and `api_key` into internal static buffers (128 and 64 bytes respectively). These are prepended to every subsequent request URL. Call once after `wifi_connect` succeeds. Does not make any network requests.

**Parameters:**
- `project_url` — full database URL, e.g. `"https://project-default-rtdb.firebaseio.com"`. No trailing slash.
- `api_key` — Web API key from Firebase Project Settings → General.

**Returns:** `ESP_OK` always (validates nothing at init time).

---

### `firebase_get`

```c
esp_err_t firebase_get(const char *path, char *out_buf, size_t buf_len);
```

Performs `GET https://<project_url><path>.json?auth=<api_key>` and writes the raw JSON response body into `out_buf`, null-terminated.

**Parameters:**
- `path` — database path starting with `/`, e.g. `"/game/state"`
- `out_buf` — caller-allocated buffer to receive the response
- `buf_len` — size of `out_buf` in bytes

**Returns:**
- `ESP_OK` — response written to `out_buf`
- `ESP_FAIL` — all retries failed, or response exceeded `buf_len`

**Critical behaviour — Firebase JSON quoting:** Firebase REST returns all values as JSON. A string value is returned with surrounding double-quotes as part of the response body:

```
GET /game/state  →  out_buf contains:  "idle"    (7 bytes including quotes)
GET /game/votes/racer_1  →  out_buf contains:  0    (bare integer, no quotes)
GET /game/favour  →  out_buf contains:  "racer_1"
```

When comparing string values, use `strstr` rather than `strcmp`:
```c
char buf[32];
firebase_get("/game/state", buf, sizeof(buf));

// Correct — works whether buf is "racing" or "\"racing\""
if (strstr(buf, "racing")) { ... }

// Wrong — fails because buf contains "racing" not racing
if (strcmp(buf, "racing") == 0) { ... }
```

When reading an integer, scan past any leading quote before calling `atoi`:
```c
char buf[16];
firebase_get("/game/votes/racer_1", buf, sizeof(buf));
const char *p = buf;
while (*p == '"' || *p == ' ') p++;
int votes = atoi(p);
```

**Buffer sizing:** Size `out_buf` for the largest response you expect at that path, plus 1 byte for the null terminator. For short string fields (`state`, `favour`) 32 bytes is sufficient. For objects (multiple fields), size accordingly — if the response exceeds `buf_len`, the function returns `ESP_FAIL` and logs a warning.

---

### `firebase_put`

```c
esp_err_t firebase_put(const char *path, const char *value_json);
```

Performs `PUT https://<project_url><path>.json?auth=<api_key>` with `value_json` as the request body. Replaces the entire node at `path`.

**`value_json` must be valid JSON:**

```c
firebase_put("/game/state", "\"racing\"");    // string — note escaped quotes
firebase_put("/game/votes/racer_1", "0");    // integer
firebase_put("/game/race/winner", "\"racer_1\"");
```

Passing a bare unquoted word like `firebase_put("/game/state", "racing")` is invalid JSON and Firebase will reject it with HTTP 400.

**Returns:** `ESP_OK` on HTTP 200, `ESP_FAIL` after retries exhausted.

---

### `firebase_patch`

```c
esp_err_t firebase_patch(const char *path, const char *patch_json);
```

Performs `PATCH` at `path`. Merges the fields in `patch_json` into the existing node — fields not listed in `patch_json` are left unchanged. Use this for multi-field atomic-ish updates to avoid separate round trips.

```c
// Set state and favour in one request
firebase_patch("/game", "{\"state\":\"racing\",\"favour\":\"racer_2\"}");

// Reset race results
firebase_patch("/game/race", "{\"start_time\":0,\"winner\":\"\",\"time_racer_1\":0,\"time_racer_2\":0}");
```

**Returns:** `ESP_OK` on HTTP 200, `ESP_FAIL` after retries exhausted.

---

### `firebase_increment`

```c
esp_err_t firebase_increment(const char *path, int delta);
```

Reads the integer at `path`, adds `delta`, and writes the result back. Implemented as GET → parse → PUT.

**Not atomic.** Two nodes calling this simultaneously may produce incorrect counts. Acceptable for vote tallying in this system (low concurrency, ~1 vote/second).

```c
firebase_increment("/game/votes/racer_1", 1);   // increment by 1
firebase_increment("/game/votes/racer_2", -1);  // decrement (if needed)
```

**Returns:** `ESP_FAIL` if the GET or PUT step fails.

---

## Retry Behaviour

All HTTP operations retry up to 3 times with 500 ms delay between attempts. Each failed attempt logs:
```
W (xxx) firebase_client: GET HTTP 401 at /game/state (attempt 1/3)
```

Common HTTP error codes from Firebase:
| Code | Cause |
|------|-------|
| 401 | Wrong or missing API key |
| 403 | Database rules deny access |
| 404 | Path doesn't exist (normal for unset nodes — returns `null`) |
| 400 | Malformed JSON in PUT/PATCH body |

After 3 failures the function returns `ESP_FAIL`. Your firmware should decide whether to abort, retry at a higher level, or continue polling.

---

## Heap and Stack Considerations

Each HTTPS request opens a TLS session which requires approximately **40–60 KB of heap** for the handshake. The ESP32-C6 has 512 KB SRAM — this is not a concern under normal operation, but avoid making concurrent Firebase calls from multiple tasks, and avoid holding large stack allocations open while a request is in flight.

Keep response buffers as local (stack) variables inside functions so they are released immediately after use. Do not declare large static buffers for Firebase responses.

When creating a task that calls Firebase functions, allocate at least **4096 bytes** of task stack:
```c
xTaskCreate(my_poll_task, "my_poll_task", 4096, NULL, 5, NULL);
```

---

## Polling Task Pattern

The standard pattern for a node that polls `/game/state`:

```c
static void state_poll_task(void *arg)
{
    char state_buf[32];
    char prev_state[32] = "unknown";

    for (;;) {
        if (firebase_get("/game/state", state_buf, sizeof(state_buf)) == ESP_OK) {
            // Use strstr — Firebase wraps strings in JSON quotes
            if (strstr(state_buf, "racing") && !strstr(prev_state, "racing")) {
                // Transition INTO racing — take action
            }
            if (strstr(state_buf, "finished") && !strstr(prev_state, "finished")) {
                // Transition INTO finished — take action
            }
            strlcpy(prev_state, state_buf, sizeof(prev_state));
        } else {
            ESP_LOGW(TAG, "Failed to read /game/state — will retry next poll");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Register in app_main or init function after firebase_init:
xTaskCreate(state_poll_task, "state_poll_task", 4096, NULL, 5, NULL);
```

Trigger on **transitions** (entering a state), not on steady state, to avoid repeating actions every 500 ms.

---

## Firebase Schema Reference

All nodes share this fixed schema. Do not add fields without team coordination.

```
/game
  /state          string   "idle" | "voting" | "racing" | "finished"
  /favour         string   "racer_1" | "racer_2" | "none"
  /votes
    /racer_1      integer
    /racer_2      integer
  /race
    /start_time   integer  Unix timestamp ms
    /winner       string   "racer_1" | "racer_2"
    /time_racer_1 integer  elapsed ms
    /time_racer_2 integer  elapsed ms
```

**State ownership** — only these nodes write `/game/state`:
- Broker: `idle→voting`, `voting→racing`, `finished→idle`
- Timekeeper: `racing→finished`

All other nodes are read-only on `/game/state`.

---

## Common Mistakes

**Calling `nvs_flash_init()` after `wifi_connect()`** — the WiFi driver reads from NVS during init. Symptom: panic/abort during `esp_wifi_init`.

**Passing unquoted strings to `firebase_put`** — `firebase_put("/game/state", "idle")` sends invalid JSON. Firebase returns HTTP 400. Use `firebase_put("/game/state", "\"idle\"")`.

**Using `strcmp` on `firebase_get` output** — Firebase wraps string values in quotes. `strcmp(buf, "idle")` always fails; use `strstr(buf, "idle")`.

**Declaring response buffers as large statics** — wastes RAM permanently. Use stack-allocated locals.

**Creating a task with less than 4096 bytes stack** — HTTPS + TLS stack usage will overflow smaller stacks silently, causing memory corruption.

**Calling `wifi_connect()` twice** — the function re-registers event handlers on every call. Calling it twice registers duplicate handlers. Only call once at startup.
