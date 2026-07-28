# CLAUDE.md — working notes for this repo

Guidance for Claude Code when working on the NixieClock firmware. User-facing
docs (features, wiring, setup) live in `README.md`; this file is about *how to
work on the code*. Read both at the start of a session.

## What this is

ESP-IDF 5.5 + PlatformIO firmware for a 6-tube multiplexed nixie clock on an
ESP32 (WROOM-32), UCB32 controller + Unidisp display board. Complete rewrite of
the old ESPHome `nixieclock.yaml` (kept only for reference). Feature-complete and
hardware-validated as of the last commit.

## Build / flash / monitor

```sh
pio run                 # build
pio run -t upload       # build + flash over USB (serial port, FTDI)
pio device monitor      # serial @ 115200
```

- **No auto-reset on the board.** Before `upload`, the user must hand-enter
  download mode: hold IO0, tap EN, release IO0. If you flash, tell them to do
  this first. "No serial data received" = they weren't in download mode (or a
  parallel upload raced — only run one upload at a time).
- **Always confirm builds before claiming success.** Capture output to a log and
  grep it: `pio run > build.log 2>&1; grep -iE "SUCCESS|FAILED|error:|warning:" build.log`.
  The repo builds **warning-clean** under `-Wall -Wextra`; keep it that way.
  Delete the temp log afterward.
- **Stale CMake cache:** after adding/removing a component dir, changing a
  `REQUIRES`, or editing an `idf_component.yml`, do `rm -rf .pio/build/esp32dev`
  then rebuild — otherwise you get bogus "No dependencies" + `fatal error: X.h:
  No such file` and managed components don't fetch.
- 4 MB flash, custom `partitions.csv`: **dual-OTA** — two ~1.875 MB app slots
  (`ota_0`/`ota_1`) + `otadata` + NVS + SPIFFS. App currently ~55% of a slot.
  Web-UI firmware upload (`POST /api/ota` in `webui.c`) streams a `.bin` into the
  spare slot via `esp_ota_*` and reboots into it; NVS (settings + creds, fixed at
  0x9000) is untouched. First flash after the repartition must still be over
  serial. **Don't shrink a slot below the current binary + BLE headroom** — a
  future NimBLE build targets ~1.4 MB.

## Architecture (the important part)

`src/main.c` orchestrates: it owns `app_main()`, the boot sequence, and the
display draw loop (priority order: ringing alarm → IP readout → anti-poison →
temperature interlude → time/slot-machine). Everything else is a `components/*`
module with a narrow header.

**The control core is the key abstraction.** `components/control` is the single
place that (a) maps a settings `key=value` → the settings store
(`control_apply_kv`) and (b) renders full device state as JSON
(`control_state_json`). MQTT, the web UI, and REST all go through it. **When you
add a setting, you edit ONE switch and ONE JSON builder here** — not three
surfaces. Then add the entity/control in `mqttctrl` (HA discovery) and `webui`
(HTML field) as presentation only.

Data flow: a surface calls `control_apply_kv` → `settings_set` (persists to NVS,
bumps a version counter) → `main.c` polls `settings_version()` each loop and
re-applies derived state (brightness, anti-poison, temps). So all surfaces stay
in sync automatically; never push display state from a surface directly.

Components: `common` (pins + secrets header-only), `nixie` (GPTimer multiplex +
PWM brightness — **do not reorder the cathode-before-anode writes or remove the
dead-time**, that's the ghosting fix), `i2cbus` (shared I²C + scanner), `rtcdev`
(PCF8563/DS1307/DS3231 auto-detect), `netclock` (WiFi+SNTP), `netcfg`
(credential store), `provision` (SoftAP portal), `settings`, `control`,
`mqttctrl`, `webui`, `temps` (slots + DS18B20 via RMT 1-Wire), `buttons`,
`alarm`, `buzzer`, `antipoison`.

The `alarm` component exposes more than ring state: `alarm_status_str(c, lt, out, n)`
builds a human string (`disabled` / `ringing` / `snoozed 6m` / `off 23h59m` /
`armed 7h12m`, honouring the DOW mask for time-to-next) consumed by BOTH
`control_state_json` and `mqttctrl publish_state` (plus an HA "Alarm status"
sensor). `alarm_preview_melody()` plays the selected melody once — wired as the
`alarm_test` action over REST/MQTT, with a web **Play** button and an HA **Test
melody** button. `alarm_is_snoozed()` feeds the colon cue (below).

## Conventions / gotchas

- **Component naming:** the ESP-IDF global namespace is huge. Don't name a public
  header, component dir, or exported symbol like an IDF one. Burned us already:
  `rtc.h`→`rtc_clock.h`, dir `rtc`→`rtcdev`, fn `rtc_init`→`rtc_open`. `main`'s
  `REQUIRES` only needs its *direct* deps; transitive ones link fine.
- **New files:** write them in one `Write` call. Sequential `Edit`s on a
  just-created file have interleaved/scrambled content this session — and `Read`
  output was occasionally garbled too. Prefer `Grep` to verify a specific line.
- **Task stacks vs. state-JSON buffers (burned us):** the full device-state JSON
  (`control_state_json` / `mqttctrl publish_state`) is built in a ~1.5 KB stack
  buffer, plus per-entity discovery buffers (~800 B) and the DS-sensor list. The
  default 4 KB FreeRTOS task stack overflows under that. The three surfaces that
  build state run with bumped stacks: **webui httpd `cfg.stack_size = 8192`**,
  **`mqtt_state` task = 8192** (`xTaskCreate`), and the **esp-mqtt client task
  `.task.stack_size = 8192`** (set in `build_cfg`, it runs the connect/command
  event handler that publishes discovery + state). If you grow the state payload
  or add entities, watch these — a "stack overflow in task httpd/mqtt_state"
  panic is this, not a logic bug. Prefer bumping the stack over `static` buffers
  (mqtt_state and the client task can both call `publish_state` → races).
- **Credentials policy (important, recently fixed):** `secrets.h`
  (`components/common/include/`, gitignored) is a **first-boot fallback ONLY**.
  Once a WiFi SSID is in NVS (device provisioned), `netcfg` trusts NVS
  exclusively — an empty key means empty (blank MQTT host = no MQTT, client goes
  idle). Don't reintroduce unconditional fallback; it makes factory reset
  silently reconnect to the compiled-in network/broker.
- **Factory reset** (`netcfg_factory_reset`) erases NVS *and* sets the one-shot
  `net/provreq` flag so the next boot enters SoftAP regardless of the secrets.h
  fallback. Destructive actions (factory reset) require a token (`"RESET"`)
  before firing, so a stray/retained MQTT message can't trigger them.
- **Tubes have NO decimal points** — the colon is the only separator (between
  MM:SS = tubes 3|4) and doubles as the decimal point in temperature mode. No
  minus glyph either: negatives are shown by blinking the whole reading.
- **Alarm colon cue (don't regress):** the colon also signals alarm state, keyed
  off `cfg.alarm_enabled`, **NOT** `alarm_is_armed()`. Tying it to armed (which
  goes false after a dismiss-for-today) made the "off for 23h59m" window revert
  to the alarm-off pattern until reboot — that was the bug. Patterns (in the
  `src/main.c` draw loop): disabled = normal blink per `blink_colon`; enabled =
  one short pip/sec; snoozed (`alarm_is_snoozed()`) = double pip; ringing = whole
  display flashes.
- **Buttons sit on tube slots 1..5** (slot 0 has no button): 1=bright+, 2=bright−,
  3=show-temp, 4=anti-poison, 5=provision (short=show IP, ~3s=provision,
  ~10s=factory reset). Snooze is a dedicated GPIO.
- **Settings schema:** bump `SETTINGS_VERSION` in `settings.h` when you change
  `clock_settings_t`; a version/size mismatch resets to defaults (creds are in a
  separate NVS namespace, so they survive).

## Git

**Public on GitHub: `github.com/Majklzbastlirny/NixieClock`, default branch
`main` (`origin`).** Commit working states with clear messages + the
Co-Authored-By trailer; push only when the user asks.

- `gh` CLI is installed at `C:\Program Files\GitHub CLI\gh.exe` (authed as
  `Majklzbastlirny`). The `!` prompt prefix runs **bash**, so `gh` calls with
  spaces/`&` fail there — invoke it from the PowerShell tool with the full path:
  `& "C:\Program Files\GitHub CLI\gh.exe" ...`.
- **Repo is public — be paranoid about secrets.** `git ls-files --cached | grep
  -i secrets.h` must show only `secrets.example.h` (the safe template); the real
  `secrets.h` must never be staged. Gitignored: `secrets.h`, `.pio/`, `.claude/`,
  build artifacts, and KiCad transients (`*.lck`, `_autosave-*`,
  `#auto_saved_files#`).
- **Commit messages:** in the Bash tool use repeated `-m` flags. PowerShell-style
  `@'...'@` here-strings leak literal chars there (bit us once — a stray `@` line
  in the subject).

## Docs & repo layout

- User-facing docs are split: `README.md` (overview, wiring, build) and
  **`GUIDE.md`** (bilingual EN/CZ — *For everyone* + *Power users*: settings
  keys, REST actions, MQTT topics, melody table). Keep both current when
  user-visible behaviour changes; they duplicate some facts (e.g. the settings
  key list) so update in lockstep.
- `3D_Files/` = printable PETG/TPU enclosure (`.3mf`) + `renders/` previews +
  `_render.py` (throwaway matplotlib/trimesh helper; run from a temp venv, not
  the build). Its own `README.md` has the parts table + assembly hardware.
- **Licensing is 3-way and scoped — never widen it:** firmware = **GPLv3** (root
  `LICENSE`); the **UCB32** board (user's own design) = **CERN-OHL-S v2**
  (`PCB_Sources/ucb vibecheck_final/LICENSE.txt`); **Unidisp**
  (`PCB_Sources/Unidisp-main/`) is @Unipuls80's third-party board under its own
  **CERN-OHL-W** — do NOT relicense or move a license over it.

## Hardware quick ref

ESP32, RTC = **PCF8563 @ I²C 0x51** (driver also handles DS @ 0x68). 2× DS18B20
on GPIO13 (needs 4.7 kΩ pull-up). Full pin map: `components/common/include/pins.h`.
HV supply has no dim input → brightness is firmware multiplex duty-cycling.
Flash over the USB serial port (FTDI), 115200 monitor.

## State / what's left

All planned milestones + polish are done (display, SNTP+RTC, brightness/night,
anti-poison, temps + DS18B20 incl. live web assign/unassign + per-sensor HA
entities, buttons, alarm+RTTTL, MQTT+HA, web UI, REST, provisioning, factory
reset). Recent additions: alarm status string + melody preview across web/HA,
the `alarm_enabled`-driven colon cue (with snooze double-pip), and **dual-OTA +
web firmware upload** (see the flash-layout note above). Repo is public with
`README.md` + bilingual `GUIDE.md` and the 3-way license in place.

Remaining ideas are optional and user-driven: REST GET aliases, BH1750
auto-brightness (hook exists: `settings_effective_brightness` takes a sensor arg,
currently `NO_SENSOR`), and **Shelly BLU H&T over BLE** (planned next: enable
NimBLE observer-only, new `components/blescan` decoding BTHome v2 ads, a
`TEMP_SRC_SHELLY` source + web picker — mirrors the DS18B20 flow; adapt from
`../MAX7219_Display/src/bthome.cpp`). Confirm scope with the user before building.
