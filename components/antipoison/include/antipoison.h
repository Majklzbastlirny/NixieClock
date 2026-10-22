// Anti-cathode-poisoning routine.
//
// Nixie cathodes that rarely light up (e.g. the leading hours digit, which on a
// 24h clock is mostly 0/1/2) develop a haze that dims them permanently. The fix
// is to periodically light EVERY cathode: this module takes over the display
// every so often and runs all tubes through 0-9 at full brightness, then hands
// the display back to the clock.
//
// Usage: call antipoison_init() once, then antipoison_tick(now_ms, may_start)
// every main loop iteration. When it returns true it has already drawn this frame
// and owns the tubes — the caller must NOT draw the time over it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void antipoison_init(void);

// Drive the routine. `now_ms` = monotonic uptime in ms (esp_timer based).
// `may_start` gates only an AUTOMATIC run that is due: main passes false while
// a scrub starting now would cover a temp/date interlude (the "pause for
// interludes" option), and the run waits until it fits. A manual trigger always
// starts at once. Returns true while the routine is active (it drew this frame).
bool antipoison_tick(int64_t now_ms, bool may_start);

// Start the routine immediately (web/REST/MQTT/button "scrub now").
void antipoison_trigger(void);

// Config. period_s = 0 disables automatic runs but leaves manual triggering
// working. step_ms = animation speed (ms per step). anim = ap_anim_t.
void antipoison_set_period_s(uint32_t period_s);
void antipoison_set_duration_ms(uint32_t duration_ms);
void antipoison_set_step_ms(uint32_t step_ms);
void antipoison_set_anim(uint8_t anim);

// ap_anim_t <-> name ("cascade", "sync", "random", "reels", "wave", "mix").
// Unknown names map to cascade.
const char *antipoison_anim_name(uint8_t anim);
uint8_t antipoison_anim_from_name(const char *name);

// Human status for the UI: "running", "off", "in 7m", "waiting for a gap".
void antipoison_status_str(char *buf, size_t n);
