// Physical buttons: debounce + short/long-hold classification.
//
// Two input paths feed this module:
//   - The multiplexed KEY line (GPIO14): the nixie driver samples it per tube
//     slot, so each tube index 0..5 is a potential button. Five of those slots
//     carry buttons (4 clock functions + 1 provisioning/reset).
//   - The dedicated snooze button on GPIO15 (not multiplexed).
//
// Which tube index is wired to which physical button is board-specific, so the
// index->action map lives as #defines in buttons.c and a discovery log prints
// the index on every press to make that mapping easy to determine on hardware.
//
// Events are delivered through a callback registered at init. buttons_tick()
// must be called regularly (every main-loop iteration) with a monotonic ms
// timestamp; all debounce/timing is done there (no separate task).
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BTN_BRIGHT_UP,       // short press
    BTN_BRIGHT_DOWN,     // short press
    BTN_SHOW_TEMP,       // short press: show temperature slots now
    BTN_ANTIPOISON,      // short press: trigger a cathode scrub
    BTN_ALARM_TOGGLE,    // bright+ and bright- held together: toggle alarm on/off
    BTN_SHOW_IP,         // short tap on the provisioning button: show the IP
    BTN_PROVISION,       // ~3s hold on the provisioning button: enter SoftAP
    BTN_FACTORY_RESET,   // ~10s hold on the provisioning button: wipe NVS
    BTN_SNOOZE_SHORT,    // snooze button tapped
    BTN_SNOOZE_LONG,     // snooze button held (dismiss)
} button_event_t;

typedef void (*button_cb_t)(button_event_t ev);

// Configure inputs and remember the callback. Call once after nixie_init().
void buttons_init(button_cb_t cb);

// Sample + debounce + classify. Call every loop with esp_timer ms uptime.
void buttons_tick(int64_t now_ms);
