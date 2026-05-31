// Nixie display driver: jitter-free multiplex refresh with PWM brightness.
//
// The refresh runs entirely off a hardware GPTimer ISR, so the rest of the
// firmware just writes a small framebuffer (which digits, dots, colon) and a
// brightness level — it never has to babysit timing. This is the key thing the
// old ESPHome version couldn't do: because brightness here is the fraction of
// each tube's time-slot that its anode stays lit, it's free and smooth.
//
// Framebuffer indexing is human-left-to-right:
//   idx 0 = leftmost tube (hours-tens) ... idx 5 = rightmost (seconds-ones).
// The driver maps that to the board's reversed select code internally.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pins.h"

#define NIXIE_BLANK 0xFF   // write this as a digit to blank a single tube

// Initialise GPIO and start the refresh timer. Call once at boot.
void nixie_init(void);

// --- Framebuffer ---------------------------------------------------------
// Set a single tube (value 0-9, or NIXIE_BLANK). idx 0 = leftmost.
void nixie_set_digit(int idx, uint8_t value);
// Set all tubes at once from a left-to-right array.
void nixie_set_digits(const uint8_t values[NIXIE_NUM_TUBES]);
// Per-tube decimal point.
void nixie_set_dot(int idx, bool on);
// The ':' separator LEDs (steady on/off; blink is done by the caller).
void nixie_set_colon(bool on);

// --- Brightness ----------------------------------------------------------
// 0 = display fully off, 255 = brightest. Applied as multiplex duty-cycle.
void nixie_set_brightness(uint8_t level);
uint8_t nixie_get_brightness(void);

// --- Multiplexed buttons -------------------------------------------------
// True while the front button sharing tube `idx`'s slot is held (debouncing
// is the caller's job; this is the raw, already-multiplex-sampled level).
bool nixie_key_pressed(int idx);
