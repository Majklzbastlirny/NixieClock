// Central, persisted settings store for the clock.
//
// One struct holds everything the user can tune; it lives in RAM behind a mutex
// and is mirrored into NVS so it survives reboots. Every subsystem that needs a
// setting (display brightness, night schedule, anti-poison, alarm, …) reads a
// snapshot via settings_get(); anything that changes a setting (web UI, MQTT,
// REST, buttons) builds a new struct and calls settings_set(), which persists it
// and bumps a version counter so pollers can notice the change cheaply.
//
// Design notes for the road ahead:
//   - Behavioural settings live in a single versioned blob (this struct). Adding
//     a field bumps SETTINGS_VERSION; on a version/size mismatch we fall back to
//     defaults rather than trusting a stale layout.
//   - Credentials (WiFi/MQTT) are deliberately NOT in this blob — they'll be
//     stored as separate NVS string keys by the provisioning step, so a firmware
//     update that grows this struct never wipes the user's WiFi password.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_MAGIC   0x4E58u   // 'NX'
#define SETTINGS_VERSION 2

#define TEMP_SLOTS 2

// Source feeding a temperature display slot.
typedef enum {
    TEMP_SRC_NONE    = 0,
    TEMP_SRC_MQTT    = 1,   // value pushed in over MQTT (HA automation)
    TEMP_SRC_DS18B20 = 2,   // a 1-Wire sensor selected by ROM address
} temp_src_t;

typedef struct {
    uint16_t magic;            // SETTINGS_MAGIC — guards against foreign/old blobs
    uint16_t version;          // SETTINGS_VERSION

    // --- Display -----------------------------------------------------------
    uint8_t  brightness;       // manual / daytime level, 0..255
    bool     night_enabled;    // dim during the night window
    uint8_t  night_brightness; // level inside the night window, 0..255
    uint16_t night_start_min;  // window start, minutes since midnight (20:30=1230)
    uint16_t night_end_min;    // window end (06:00=360); wraps past midnight
    bool     auto_brightness;  // future BH1750: sensor drives level when true
    uint8_t  bright_min;       // auto-brightness clamp, low
    uint8_t  bright_max;       // auto-brightness clamp, high
    bool     blink_colon;      // blink the ':' at 1 Hz vs. steady on
    bool     h24;              // 24-hour format (false = 12-hour, blanks leading 0)

    // --- Anti-cathode-poisoning -------------------------------------------
    bool     ap_enabled;
    uint32_t ap_period_s;      // run every N seconds (0 = off)
    uint32_t ap_duration_ms;   // length of each run

    // --- Alarm (single alarm for now; array later) ------------------------
    bool     alarm_enabled;
    uint8_t  alarm_hour;       // 0..23
    uint8_t  alarm_min;        // 0..59
    uint8_t  alarm_dow_mask;   // bit0=Sun … bit6=Sat; 0 = every day
    uint8_t  alarm_melody;     // index into the RTTTL melody table
    uint8_t  alarm_snooze_min; // snooze length in minutes

    // --- Temperature display (brief interlude near the end of each minute) -
    // Slot 0 shows at seconds 52-53, slot 1 at 54-55, then time resumes at 56.
    bool     temp_enabled;                 // master enable for the temp slots
    uint8_t  temp_decimals;                // digits after the point: 0, 1 or 2
    uint8_t  temp_src[TEMP_SLOTS];         // per slot: temp_src_t
    uint8_t  temp_rom[TEMP_SLOTS][8];      // DS18B20 ROM code (src == DS18B20)
} clock_settings_t;

// Load from NVS (or defaults if absent/corrupt) and prepare the lock. Also
// performs the one-time nvs_flash_init for the whole app. Call once, early.
void settings_init(void);

// Thread-safe snapshot into *out.
void settings_get(clock_settings_t *out);

// Replace the live settings with *in, persist to NVS, and bump the version.
// (magic/version fields are set for you.) Returns ESP_OK on a successful save.
int settings_set(const clock_settings_t *in);

// Monotonic counter, incremented on every successful settings_set(). Pollers
// keep the last value they saw and re-apply derived state when it changes.
uint32_t settings_version(void);

// --- Pure display-policy helpers (operate on a snapshot, no locking) --------
// True if minute-of-day `mins` (0..1439) falls in the night window.
bool settings_is_night(const clock_settings_t *s, int mins);
// Effective brightness for the current minute. `sensor` is a BH1750 reading
// (0..255) or negative when no sensor is present/enabled.
uint8_t settings_effective_brightness(const clock_settings_t *s, int mins, int sensor);
