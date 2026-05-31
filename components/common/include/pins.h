// Central pin map for the NixieClock (UCB32 controller + Unidisp display board).
//
// The display is a 6-tube, time-multiplexed nixie panel. One tube is lit at a
// time; the firmware walks through them fast enough to look continuous.
//
//   Tube select (74x42, "S0..S2" on the Unidisp X2 header):
//     code 0 = RIGHTMOST tube ... code 5 = LEFTMOST tube, code 6/7 = display off.
//   Digit value (MH74141, "D0..D3"): BCD. A value >= 10 (e.g. 0xF) blanks the
//     selected tube without disabling its anode — handy for per-tube dimming
//     while still reading that tube's multiplexed button.
//
// All Unidisp logic is 5V TTL but reads the ESP32's 3.3V output as logic high.
// KEY is open-drain active-low and idles high through the ESP's internal pullup.
#pragma once

#include "driver/gpio.h"

// --- Tube select (anode multiplex, 74x42 address) ---
#define PIN_SEL0        GPIO_NUM_16   // S0 / LSB
#define PIN_SEL1        GPIO_NUM_17   // S1
#define PIN_SEL2        GPIO_NUM_18   // S2 / MSB

// --- Digit value (cathode, MH74141 BCD) ---
#define PIN_BCD0        GPIO_NUM_23   // D0 / LSB
#define PIN_BCD1        GPIO_NUM_25   // D1
#define PIN_BCD2        GPIO_NUM_26   // D2
#define PIN_BCD3        GPIO_NUM_27   // D3 / MSB

// --- Decorations ---
#define PIN_DOT         GPIO_NUM_32   // DP: per-tube decimal points (multiplexed, H = on)
#define PIN_COLON       GPIO_NUM_33   // COL: the ':' separator LEDs (not multiplexed)

// --- Inputs ---
#define PIN_KEY         GPIO_NUM_14   // multiplexed front buttons, active-low, needs pullup
#define PIN_SNOOZE      GPIO_NUM_15   // snooze button, active-low

// --- Audio ---
#define PIN_BUZZER      GPIO_NUM_19   // ALM: passive piezo, driven via LEDC

// --- I2C bus (DS3231 RTC @ 0x68; future BH1750 light sensor @ 0x23) ---
#define PIN_I2C_SDA     GPIO_NUM_21
#define PIN_I2C_SCL     GPIO_NUM_22

// --- 1-Wire (DS18B20 temperature, RMT-driven) ---
#define PIN_ONEWIRE     GPIO_NUM_13   // needs a 4.7k pull-up to 3.3V on the data line

// --- Misc ---
#define PIN_STATUS_LED  GPIO_NUM_4
#define PIN_BATT_ADC    GPIO_NUM_34   // ADC1_CH6, 22k/22k divider (x2). Unused in v1.

#define NIXIE_NUM_TUBES 6
