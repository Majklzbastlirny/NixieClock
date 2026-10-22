// shellytime — optional clock source: the BLE "Date/Time Broadcast" beacon that
// Gen2+ Shellies send (the one the Shelly BLU H&T Display ZB syncs from).
//
// Which Shellies send it: fw >= 2.0 devices that host BTHome devices or act as a
// Shelly Cloud BLE relay. Each re-stamps the time every ~10 s and sends it in a
// ~3 s burst. On air it is one manufacturer-specific AD structure, little-endian:
//
//   18 FF A9 0B 0D <u32 unix UTC> <i32 std_off s> <u32 dst_start> <u32 dst_end> <i32 dst_shift s>
//
// Company id is 0x0BA9 (Shelly). The API docs say 0xFCD2; that is wrong (real
// Shellies send 0x0BA9, and the BLU display ignores 0xFCD2).
//
// Flow:
//  - blescan hands every manufacturer AD it sees to shellytime_feed() (NimBLE
//    host task) -> a small per-sender table behind a mutex. For each new stamp
//    we keep the uptime when we FIRST heard it, so a sync can add the elapsed
//    time and lose the 0..3 s burst lag (~0.1 s left).
//  - "Sync now" (web/MQTT/REST) only raises a request (shellytime_request_sync);
//    the main loop's shellytime_tick() performs it: median of the fresh senders
//    -> settimeofday + RTC + persisted epoch (same trio as /api/settime). TZ is
//    never touched: main owns it.
//  - The optional daily sync (settings.shelly_time_daily) runs ~1 min after boot
//    and then every 24 h, but yields to NTP: it is skipped while SNTP has synced
//    within the last 24 h, so it only takes over when the network time is gone.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

// Create the lock. Call once, before blescan_start().
void shellytime_init(void);

// Offer one manufacturer-specific AD payload (company id first, as it follows the
// AD type byte). Cheap no-op for anything that isn't a Shelly time beacon. `mac`
// is little-endian as delivered by the controller.
void shellytime_feed(const uint8_t *mac, int8_t rssi, const uint8_t *mfr, int len);

// Ask for a sync now (thread-safe, returns immediately). The main loop applies
// the next usable beacon, waiting up to ~25 s for one.
void shellytime_request_sync(void);

// Main-loop hook: performs a requested sync and the optional daily one.
void shellytime_tick(const clock_settings_t *c, int64_t now_ms);

// Debug/status JSON for the web panel (GET /api/shellytime): last result, daily
// state, and per-sender RSSI / age / clock delta. Returns the length written.
size_t shellytime_status_json(char *buf, size_t n);
