// MQTT control surface with Home Assistant autodiscovery.
//
// This is the first "always-connected" remote-control layer. Once WiFi is up,
// the clock keeps a persistent MQTT session to the broker and:
//   - announces itself to Home Assistant via the MQTT discovery protocol, so a
//     "Nixie Clock" device with brightness / night-schedule / alarm controls and
//     status sensors appears automatically (no YAML in HA);
//   - publishes its state periodically and right after any change;
//   - subscribes to per-entity command topics and applies them through the
//     shared settings store (so web UI, REST and buttons all stay in sync).
//
// Credentials are resolved via netcfg (NVS, with secrets.h as fallback).
// Everything here is non-blocking: if the broker is down the clock keeps running
// and the client reconnects on its own.
#pragma once

#include <stdbool.h>

// Start the MQTT client + its state-publishing task. Call once, after
// netclock_start() (WiFi must be coming up). Safe to call if the broker is
// unreachable — it just keeps retrying in the background.
void mqttctrl_start(void);

// True while the MQTT session is connected to the broker.
bool mqttctrl_connected(void);

// Re-read MQTT credentials from netcfg and reconnect the client with them.
// Used after the web UI changes the broker settings, so the new connection
// takes effect without a reboot.
void mqttctrl_reconnect(void);
