// Template for the WiFi + MQTT credentials and the web UI login.
//
// Copy this file to `secrets.h` (same folder) and fill in your details.
// `secrets.h` is gitignored so your credentials never get committed.
//
// WiFi/MQTT here are only the COMPILE-TIME FALLBACK: once the clock is
// provisioned over the SoftAP portal or the web UI, the values stored in NVS
// take precedence and these are ignored. A board flashed with valid values here
// will connect on first boot without needing the portal.
#pragma once

#define WIFI_SSID "your-network-name"
#define WIFI_PASS "your-password"

// Optional MQTT broker (for Home Assistant). Leave host empty to disable.
#define MQTT_IP   "192.168.0.2"
#define MQTT_PORT "1883"
#define MQTT_USER "nixie"
#define MQTT_PASS "your-mqtt-password"

// Web UI login (HTTP Basic auth). It guards the whole web UI, the REST API and
// the firmware upload. Unlike WiFi/MQTT this is ALWAYS taken from here (not
// NVS), so changing it means rebuilding + flashing. Leave either one empty to
// turn the login off. Plain HTTP: it keeps others on your LAN out, it does not
// encrypt. Locked out? Hold the provision button ~3 s and use "Boot previous
// firmware" in the setup portal.
#define OTA_USERNAME "admin"
#define OTA_PASSWORD "change-me"
