// RTC driver for the clock's I2C real-time clock. Auto-detects, at open():
//   - PCF8563 @ 0x51 (time regs from 0x02, VL bit = unreliable), or
//   - DS1307 / DS3231 @ 0x68 (time regs from 0x00, CH bit = clock-halt).
// All BCD, run in 24h mode; only the common fields are touched, so swapping the
// chip needs no code change.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int year;     // full year, e.g. 2026
    int month;    // 1-12
    int day;      // 1-31
    int hour;     // 0-23 (we always run the RTC in 24h mode)
    int minute;   // 0-59
    int second;   // 0-59
    int weekday;  // 1-7 (1=Sunday, RTC convention); not used for display
} rtc_time_t;

// Probe and attach the RTC on the shared I2C bus. Returns ESP_OK if a supported
// chip ACKs. (Named rtc_open, not rtc_init: ESP-IDF already has a global rtc_init.)
esp_err_t rtc_open(void);

// True once rtc_open() has found a supported RTC chip.
bool rtc_present(void);

// Read the current time. ESP_OK on a successful I2C transaction.
esp_err_t rtc_get(rtc_time_t *out);

// Set the time (also clears the DS1307 clock-halt bit so it starts ticking).
esp_err_t rtc_set(const rtc_time_t *t);

// True if the value looks like a real, already-set time (range-checked, and the
// oscillator wasn't halted). A cold/unset DS1307 fails this -> caller shows the
// "alive but unset" slot-machine animation until SNTP sets it.
bool rtc_time_valid(const rtc_time_t *t);
