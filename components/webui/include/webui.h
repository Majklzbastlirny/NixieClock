// Self-hosted web UI + JSON API, served over HTTP on port 80.
//
// Endpoints:
//   GET  /              -> the control page (HTML, embedded in the binary)
//   GET  /api/state     -> full device state as JSON (control_state_json)
//   POST /api/set?k=&v= -> apply one settings key=value (control_apply_kv)
//   POST /api/action?a= -> antipoison_now | alarm_snooze | alarm_dismiss
//                          | temp1_input&v= | temp2_input&v=
//
// Start once WiFi is up (call after netclock_start()). The server binds to
// whatever IP the STA gets; the same handlers will back the REST milestone.
#pragma once

void webui_start(void);
