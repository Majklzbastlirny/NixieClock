// WiFi provisioning over a SoftAP captive portal.
//
// When entered, the clock stops being a station and instead hosts an open
// access point "NixieClock-XXXX" with a web form: pick a scanned SSID, type the
// password (and optionally MQTT broker settings), submit. Credentials are saved
// via netcfg and the device reboots into normal station mode.
//
// A tiny DNS responder answers every query with our AP IP, so phones that probe
// for a captive portal pop the form automatically.
//
// Entry is decided at boot (see provision_requested): either the provisioning
// button was held through the boot window, or a previous run set the "enter
// provisioning" flag (e.g. a long-press during normal operation) and rebooted.
#pragma once

#include <stdbool.h>

// True if provisioning should run this boot. Checks: (1) a one-shot NVS request
// flag set by provision_request_reboot(), cleared here; (2) the provisioning
// button (KEY slot 5) held down right now during the boot window; (3) no
// credentials stored yet (first-ever boot with no secrets.h fallback SSID).
bool provision_requested(void);

// Run the SoftAP portal. Blocks (the clock display keeps running on its own
// timer ISR); on a successful submit it saves creds and reboots, so this does
// not normally return. `node_suffix` is appended to the AP SSID/name.
void provision_run(const char *node_suffix);

// Set the one-shot "enter provisioning on next boot" flag and reboot. Called
// from the runtime provisioning button long-hold.
void provision_request_reboot(void);
