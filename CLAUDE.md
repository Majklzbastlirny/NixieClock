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
pio run -t upload       # build + flash over USB (COM51, FTDI)
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
- 4 MB flash, custom `partitions.csv` (single ~2.8 MB app + NVS + SPIFFS, no OTA).
  App currently ~37% of the slot, lots of headroom.

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

## Conventions / gotchas

- **Component naming:** the ESP-IDF global namespace is huge. Don't name a public
  header, component dir, or exported symbol like an IDF one. Burned us already:
  `rtc.h`→`rtc_clock.h`, dir `rtc`→`rtcdev`, fn `rtc_init`→`rtc_open`. `main`'s
  `REQUIRES` only needs its *direct* deps; transitive ones link fine.
- **New files:** write them in one `Write` call. Sequential `Edit`s on a
  just-created file have interleaved/scrambled content this session — and `Read`
  output was occasionally garbled too. Prefer `Grep` to verify a specific line.
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
- **Buttons sit on tube slots 1..5** (slot 0 has no button): 1=bright+, 2=bright−,
  3=show-temp, 4=anti-poison, 5=provision (short=show IP, ~3s=provision,
  ~10s=factory reset). Snooze is a dedicated GPIO.
- **Settings schema:** bump `SETTINGS_VERSION` in `settings.h` when you change
  `clock_settings_t`; a version/size mismatch resets to defaults (creds are in a
  separate NVS namespace, so they survive).

## Git

Repo is initialized; commit working states with clear messages. `secrets.h`,
`.pio/`, `.claude/`, and build artifacts are gitignored — **verify the secret is
never staged** (`git ls-files --cached | grep secrets.h` must be empty) before
committing. End commit messages with the Co-Authored-By trailer.

## Hardware quick ref

ESP32, RTC = **PCF8563 @ I²C 0x51** (driver also handles DS @ 0x68). 2× DS18B20
on GPIO13 (needs 4.7 kΩ pull-up). Full pin map: `components/common/include/pins.h`.
HV supply has no dim input → brightness is firmware multiplex duty-cycling.
Flash port COM51 (FTDI), 115200 monitor.

## State / what's left

All planned milestones + polish are done (display, SNTP+RTC, brightness/night,
anti-poison, temps + DS18B20, buttons, alarm+RTTTL, MQTT+HA, web UI, REST,
provisioning, factory reset). Remaining ideas are optional and user-driven:
REST GET aliases, BH1750 auto-brightness (hook exists: `settings_effective_
brightness` takes a sensor arg, currently `NO_SENSOR`), OTA (would need a
repartition). Confirm scope with the user before building.
