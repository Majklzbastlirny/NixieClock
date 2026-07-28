// blescan — passive BLE scan for Shelly BLU H&T (BTHome v2) temperature.
//
// Uses NimBLE in the observer role (never advertises/connects). Every BTHome v2
// advertisement is decoded; a small registry of heard devices backs the web
// picker, and any device the user has assigned to a temperature slot (source
// TEMP_SRC_SHELLY) feeds that slot via temps_set_shelly().
//
// Only unencrypted BTHome v2 is handled (Shelly BLU default). WiFi and BLE share
// the one radio via software coexistence (enabled in sdkconfig).
#pragma once

#include <stddef.h>
#include "settings.h"

// Bring up NimBLE and start the perpetual passive scan. Call once at boot.
void blescan_start(void);

// Cache which temperature slots are assigned to a Shelly MAC, so the scan
// callback can route matching advertisements. Call at boot and whenever settings
// change (right beside temps_apply_settings). Safe before blescan_start() (no-op).
void blescan_apply_settings(const clock_settings_t *c);

// Emit the discovered-device list as a JSON array for the web picker:
//   [{"mac","name","rssi","age","ht","hh","hb","t","h","b"}, ...]
// (temp/hum/batt fields present only when that quantity was seen). Devices not
// heard for >10 min are hidden. Returns the byte length written.
size_t blescan_devices_json(char *buf, size_t n);
