// WiFi (station) + SNTP time sync.
//
// WiFi here exists for one job right now: get the time. On connect we start
// SNTP; on each successful sync we push the time into the RTC (so the clock
// keeps running accurately even when WiFi later drops). Timezone is applied via
// the standard POSIX TZ string so localtime_r() gives wall-clock time.
//
// Designed to be non-blocking: call netclock_start() once and forget it. The
// display keeps running off the RTC regardless of WiFi state.
#pragma once

#include <stdbool.h>

// Kick off WiFi + SNTP in the background. Safe to call once at boot.
void netclock_start(void);

// True once we've had at least one successful SNTP sync this power cycle.
bool netclock_synced(void);
