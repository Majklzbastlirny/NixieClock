// Alarm state machine: fires at the configured time, rings an RTTTL melody on
// the buzzer, supports snooze (re-rings after N minutes) and dismiss (silence
// until the next day).
//
// Display effects (flash while ringing, shortened-colon "armed" cue, the
// two-blink dismiss confirmation) are owned by main.c; this module just exposes
// the state it needs to render and the actions the buttons/MQTT invoke.
//
// Drive it by calling alarm_tick() every loop with the current local time and a
// settings snapshot. All timing is monotonic-ms based (esp_timer).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "settings.h"

void alarm_init(void);

// Advance the state machine. `lt` = current local time, `c` = settings snapshot,
// `now_ms` = esp_timer uptime in ms.
void alarm_tick(const struct tm *lt, const clock_settings_t *c, int64_t now_ms);

// True while the buzzer is actively sounding (use to flash the display).
bool alarm_is_ringing(void);

// True when the alarm is enabled and still pending for today (not dismissed) —
// used to show the "armed" colon cue.
bool alarm_is_armed(void);

// Snooze a ringing alarm for the configured minutes. No-op if not ringing.
void alarm_snooze(void);

// Dismiss the alarm for the rest of today (stops ringing/snooze). Returns true
// if it actually silenced something (so the caller can play a confirm blink).
bool alarm_dismiss(void);
