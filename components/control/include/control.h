// Shared control core: one place that maps a (key, value) string pair onto the
// settings store, and one place that renders the full device state as JSON.
//
// Every control surface — MQTT, the web UI, the REST API — speaks the same
// vocabulary by going through here, so adding a setting means editing exactly
// one switch and one JSON builder instead of three.
//
// Scope: control_apply_kv handles the *settings* keys (the part that keeps
// growing). Transport-specific actions (antipoison_now, alarm snooze/dismiss,
// pushing an MQTT temperature value) stay in their respective surfaces, since
// they're a small, stable set and sometimes need transport-local echo state.
#pragma once

#include <stddef.h>
#include <stdbool.h>

// Apply one settings key=value. `vlen` is the value length (for ON/OFF and
// empty-string handling). Returns true if `key` was a recognised setting (the
// value is applied and persisted); false if the key is unknown, so the caller
// can try its own handlers. Bad values for a known key are clamped/ignored and
// still return true.
bool control_apply_kv(const char *key, const char *val, int vlen);

// Render the full device state (settings + computed status: night active, time
// synced, alarm ringing/armed, temperature readings, discovered DS18B20 ROMs)
// as a JSON object into buf. Returns the number of bytes written (excl. NUL).
int control_state_json(char *buf, size_t n);
