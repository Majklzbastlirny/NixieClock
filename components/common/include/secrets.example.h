// Template for WiFi + MQTT credentials.
//
// Copy this file to `secrets.h` (same folder) and fill in your details.
// `secrets.h` is gitignored so your credentials never get committed.
//
// These are only the COMPILE-TIME FALLBACK: once the clock is provisioned over
// the SoftAP portal or the web UI, the values stored in NVS take precedence and
// these are ignored. A board flashed with valid values here will connect on
// first boot without needing the portal.
#pragma once

#define WIFI_SSID "your-network-name"
#define WIFI_PASS "your-password"

// Optional MQTT broker (for Home Assistant). Leave host empty to disable.
#define MQTT_IP   "192.168.0.2"
#define MQTT_PORT "1883"
#define MQTT_USER "nixie"
#define MQTT_PASS "your-mqtt-password"
