// Temperature slots for the brief end-of-minute display.
//
// There are TEMP_SLOTS readings, each fed by a source chosen in settings:
//   - TEMP_SRC_MQTT:    a value pushed in over MQTT (temps_set_mqtt), e.g. an
//                       HA automation writing the outside temperature.
//   - TEMP_SRC_DS18B20: a 1-Wire sensor picked by ROM address. The bus is
//                       scanned at boot; assign a discovered ROM to a slot and a
//                       background task polls it (RMT-driven, no display impact).
//   - TEMP_SRC_NONE:    slot disabled.
//
// Values are kept in centi-degrees Celsius (int16): 2345 = 23.45 C, -534 = -5.34 C.
// A reading is only handed out by temps_get() while it is fresh (see FRESH_MS in
// the .c); stale data is treated as "no reading" so the clock falls back to
// showing the time rather than a frozen temperature.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "settings.h"

#define TEMP_CENTI_INVALID INT16_MIN
#define TEMP_ROM_STRLEN    17        // 16 hex chars + NUL

// Prepare storage + lock. Call once at boot, before temps_apply_settings().
void temps_init(void);

// Bring up the 1-Wire bus, scan for DS18B20 sensors, and start the poll task.
// Call once after temps_init()/temps_apply_settings(). Safe if no bus/sensor is
// present (it just finds nothing and keeps retrying scans).
void temps_start(void);

// Re-read source/ROM/decimals config from settings (call at boot and whenever
// settings change).
void temps_apply_settings(const clock_settings_t *c);

// Latest fresh reading for `slot` in centi-degrees C. Returns false if the slot
// is disabled, has never been written, or the value is stale.
bool temps_get(int slot, int16_t *centi_out);

// Push an MQTT-sourced value (centi-degrees C) into a slot. Ignored unless that
// slot's source is TEMP_SRC_MQTT.
void temps_set_mqtt(int slot, int centi);

// List ROM addresses currently found on the 1-Wire bus. `out` is an array of
// TEMP_ROM_STRLEN-char buffers. Returns the count written (capped at max).
int temps_list_roms(char out[][TEMP_ROM_STRLEN], int max);

// Bumped whenever the set of discovered ROMs changes, so callers (MQTT) can
// refresh any "pick a sensor" UI.
uint32_t temps_scan_generation(void);

// ROM <-> 16-char hex string helpers. parse returns false on malformed input.
void temps_rom_to_str(const uint8_t rom[8], char out[TEMP_ROM_STRLEN]);
bool temps_parse_rom(const char *s, uint8_t out[8]);
