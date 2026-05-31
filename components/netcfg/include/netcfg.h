// Network credentials store: WiFi + MQTT settings that live in NVS, with the
// compile-time secrets.h values as a fallback.
//
// Resolution order for every field: NVS value if present and non-empty, else
// the secrets.h #define. So a freshly-flashed board still connects using your
// secrets.h defaults, but once the clock is provisioned over SoftAP (or a friend
// re-provisions it) the NVS values take over — no rebuild needed, and the
// secret never has to be edited.
//
// Credentials are kept in their own NVS namespace ("net"), separate from the
// behavioural settings blob, so growing clock_settings_t never wipes them and a
// factory reset (which erases all NVS) clears them together with everything else.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_host[64];
    char mqtt_port[6];
    char mqtt_user[33];
    char mqtt_pass[65];
} netcfg_t;

// Populate *out with the effective config (NVS overlaid on secrets.h fallback).
void netcfg_get(netcfg_t *out);

// Persist WiFi credentials to NVS. Either may be NULL/"" to leave unchanged.
// Returns true on a successful write.
bool netcfg_set_wifi(const char *ssid, const char *pass);

// Persist MQTT settings to NVS (any NULL field is left unchanged).
bool netcfg_set_mqtt(const char *host, const char *port,
                     const char *user, const char *pass);

// True if WiFi credentials have been stored in NVS (i.e. the device has been
// provisioned at least once, as opposed to running on secrets.h defaults).
bool netcfg_is_provisioned(void);
