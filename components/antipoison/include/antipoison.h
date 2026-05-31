// Anti-cathode-poisoning routine.
//
// Nixie cathodes that rarely light up (e.g. the leading hours digit, which on a
// 24h clock is mostly 0/1/2) develop a haze that dims them permanently. The fix
// is to periodically light EVERY cathode: this module takes over the display
// every so often and runs all tubes through 0-9 at full brightness, then hands
// the display back to the clock.
//
// Usage: call antipoison_init() once, then antipoison_tick(now_ms) every main
// loop iteration. When it returns true it has already drawn this frame and owns
// the tubes — the caller must NOT draw the time over it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void antipoison_init(void);

// Drive the routine. `now_ms` = monotonic uptime in ms (esp_timer based).
// Returns true while the routine is active (it has written the framebuffer).
bool antipoison_tick(int64_t now_ms);

// Start the routine immediately (e.g. a future web/REST "scrub now" button).
void antipoison_trigger(void);

// Config (web UI / NVS later). period_s = 0 disables automatic runs but leaves
// manual triggering working. Defaults: period 600s, duration 12s.
void antipoison_set_period_s(uint32_t period_s);
void antipoison_set_duration_ms(uint32_t duration_ms);
