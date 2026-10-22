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
#define SETTINGS_VERSION 6

#define TZ_STRLEN 40               // POSIX TZ string buffer (e.g. CET-1CEST,...)

#define TEMP_SLOTS 2

// How the display decides when/how to dim (the "night mode").
typedef enum {
    NIGHT_MODE_FIXED = 0,   // dim inside a fixed clock-time window
    NIGHT_MODE_SUN   = 1,   // dim between sunset and sunrise (computed on-device)
    NIGHT_MODE_LUX   = 2,   // track a BH1750 ambient-light sensor (if present)
} night_mode_t;

// Source feeding a temperature display slot.
typedef enum {
    TEMP_SRC_NONE    = 0,
    TEMP_SRC_MQTT    = 1,   // value pushed in over MQTT (HA automation)
    TEMP_SRC_DS18B20 = 2,   // a 1-Wire sensor selected by ROM address
    TEMP_SRC_SHELLY  = 3,   // a Shelly BLU H&T (BTHome v2 BLE) picked by MAC
} temp_src_t;

typedef struct {
    uint16_t magic;            // SETTINGS_MAGIC — guards against foreign/old blobs
    uint16_t version;          // SETTINGS_VERSION

    // --- Display -----------------------------------------------------------
    uint8_t  brightness;       // manual / daytime level, 0..255
    bool     night_enabled;    // master toggle: dim at night at all
    uint8_t  night_mode;       // night_mode_t: fixed window / sun / BH1750 lux
    uint8_t  night_brightness; // level inside the night window, 0..255
    uint16_t night_start_min;  // window start, minutes since midnight (20:30=1230)
    uint16_t night_end_min;    // window end (06:00=360); wraps past midnight
    int16_t  lat_e2;           // latitude  x100 (deg, N+), for NIGHT_MODE_SUN
    int16_t  lon_e2;           // longitude x100 (deg, E+), for NIGHT_MODE_SUN
    int8_t   sun_offset_min;   // sun-mode twilight offset, minutes (was the dead
                               // auto_brightness byte — repurposed, same size, so
                               // no schema bump). +ve = stay bright longer into
                               // dusk/dawn twilight; -ve = dim earlier.
    uint8_t  shelly_time_daily; // 1 = daily clock sync from the Shelly BLE time
                               // beacon (shellytime; yields to NTP). Was the
                               // reserved bright_min byte, always 0 in stored
                               // blobs: repurposed, same size, no schema bump.
    uint8_t  bright_max;       // (reserved / unused)
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
    uint8_t  shelly_mac[TEMP_SLOTS][6];    // Shelly BLU BLE MAC (src == SHELLY)
    int16_t  temp_offset[TEMP_SLOTS];      // per-slot calibration, centi-degrees C

    // Interlude scheduling: each shows during [start, start+len) seconds of the
    // minute (mod 60). Precedence temp0 -> temp1 -> date if windows overlap.
    uint8_t  temp_sec[TEMP_SLOTS];         // per-slot start second (0..59)
    uint8_t  temp_len[TEMP_SLOTS];         // per-slot length in seconds (1..8)
    bool     date_enabled;                 // show the date interlude
    uint8_t  date_sec;                     // date start second
    uint8_t  date_len;                     // date length in seconds

    // --- Timezone ----------------------------------------------------------
    char     tz[TZ_STRLEN];                // POSIX TZ string (DST-aware)
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

// Persist / recall the last-known wall-clock epoch (Unix seconds) to NVS, under
// a key SEPARATE from the versioned settings blob so it survives schema bumps.
// Used to reseed the clock (approximately) when the RTC has lost power, instead
// of booting to 1970. settings_load_epoch returns false if nothing is stored.
void settings_persist_epoch(int64_t sec);
bool settings_load_epoch(int64_t *sec);

// RTC storage-format marker (NVS key "rtc_utc", also outside the blob). Older
// firmware kept LOCAL time in the RTC chip, which goes an hour wrong across a
// DST change; current firmware keeps UTC. Absent = legacy contents, which
// rtcdev reads once as local time and rewrites as UTC (then marks this).
bool settings_rtc_is_utc(void);
void settings_mark_rtc_utc(void);

// --- Pure display-policy helpers (operate on a snapshot, no locking) --------
// True if minute-of-day `mins` (0..1439) falls in the fixed night window.
bool settings_is_night(const clock_settings_t *s, int mins);

// True if the sun is below the horizon at UTC epoch `utc` (Unix seconds), for
// the configured lat/lon. Fully offline — pure astronomy, no network. Used by
// NIGHT_MODE_SUN. (Epoch is int64_t, not time_t, to keep this widely-included
// header free of <time.h>.)
bool settings_sun_is_night(const clock_settings_t *s, int64_t utc);

// True if it is currently "night" for the active mode (fixed window, or sun
// below horizon for sun/lux). Always false when night dimming is disabled.
// Used only for the "night active" status readout.
bool settings_is_night_now(const clock_settings_t *s, int64_t utc, int mins);

// Effective display brightness right now. `mins` = local minute-of-day (fixed
// window), `utc` = epoch seconds (sun calc), `lux` = BH1750 reading or -1.
uint8_t settings_brightness_for(const clock_settings_t *s, int64_t utc,
                                int mins, int lux);
