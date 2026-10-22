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
- **Firmware version string is baked in at CMake configure time** (`git describe`,
  shown as `fw` in `/api/state`). An incremental build after a commit keeps the
  old `<hash>-dirty`; to flash an image that reports the new commit, commit first,
  then `rm -rf .pio/build/esp32dev` and rebuild.
- 4 MB flash, custom `partitions.csv`: **dual-OTA** — two ~1.875 MB app slots
  (`ota_0`/`ota_1`) + `otadata` + NVS + SPIFFS. App currently ~67% of a slot (1.31 MB).
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
`mqttctrl`, `webui`, `temps` (slots + DS18B20 via RMT 1-Wire), `blescan`
(NimBLE passive scan → Shelly BLU H&T over BTHome v2; feeds a temp slot via
`temps_set_shelly`/`temps_set_shelly_battery` when a slot's source is
`TEMP_SRC_SHELLY` and its `shelly_mac` matches — unencrypted BTHome only. Also
decodes the button event 0x3A → the web picker highlights the pressed sensor
["identify"], and battery ≤20% blinks the temp colon via `temps_battery`),
`buttons`,
`alarm`, `buzzer`, `antipoison`, `bh1750` (optional ambient-light sensor;
`bh1750_init()` probes I²C 0x23 then 0x5C at boot and puts it in continuous
H-res mode — `bh1750_present()`/`bh1750_read_lux()` gate + feed the `lux` night
mode), `weblog` (esp_log→RAM ring buffer, read back
over `GET /api/log` + a Logs panel — the assembled clock has no serial, so this
is how you read its logs; `weblog_init()` runs first in `app_main`),
`shellytime` (optional clock source from the BLE time beacon Gen2+ Shelly
gateways broadcast; see the Shelly BLE time note below).

**Night dimming has three modes** (`night_mode`: `fixed`/`sun`/`lux`, default
`sun`), all resolved in `settings_brightness_for(s, utc, mins, lux)` — the single
policy fn `main.c` calls each loop. `fixed` = the clock-time window; `sun` =
`settings_sun_is_night()`, a pure on-device NOAA solar-elevation calc evaluated
in **UTC** (so no TZ/DST handling) from `lat_e2`/`lon_e2` — **works fully offline**,
no API; `lux` = map a BH1750 reading to a level (falls back to `sun` if no
sensor). `night_active` in state JSON now comes from `settings_is_night_now()`,
not the old fixed-only `settings_is_night()`.

**OTA is web-upload only** (`POST /api/ota` in `webui.c`) — the assembled clock's
UART is physically disconnected. Two gotchas baked into the handler, don't
regress them: (1) it calls `nixie_hold_off(true)` for the whole flash so the
multiplexed tubes stay dark instead of glitching while `esp_ota_write` stalls
the CPU — cleared only on failure; (2) the recv loop **bounds** consecutive
socket timeouts (`OTA_MAX_RECV_STALLS` × `recv_wait_timeout`) and aborts, or a
dropped upload spins the single httpd worker forever with the display latched
blank and the device needs a power cycle (learned the hard way).

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
  minus glyph either: negatives are shown by blinking the whole reading. Low
  Shelly battery blinks *just the colon* during that temp reading (`show_temp`'s
  `batt_low` arg) — a deliberately different cue from the negative whole-reading
  blink; don't merge the two.
- **Alarm colon cue (don't regress):** the colon also signals alarm state, keyed
  off `cfg.alarm_enabled`, **NOT** `alarm_is_armed()`. Tying it to armed (which
  goes false after a dismiss-for-today) made the "off for 23h59m" window revert
  to the alarm-off pattern until reboot — that was the bug. Patterns (in the
  `src/main.c` draw loop): disabled = normal blink per `blink_colon`; enabled =
  one short pip/sec; snoozed (`alarm_is_snoozed()`) = double pip; ringing = whole
  display flashes. The pip/blink phase keys off the **wall-clock sub-second**
  (`gettimeofday`'s `tv_usec`, var `subms`), NOT boot-relative `now_ms` — so the
  colon turns on exactly on the second edge. Don't revert to `now_ms % 1000`.
- **Shelly BLE time (`shellytime`):** Gen2+ Shellies on fw 2.x that host BTHome
  devices or relay BLU devices to the cloud broadcast a time beacon:
  manufacturer AD `A9 0B 0D <u32 unix><i32 std_off><u32 dst_start><u32 dst_end><i32 dst_shift>`.
  The company id is **0x0BA9**. Shelly's API docs say 0xFCD2, which is wrong, and the
  BLU display ignores 0xFCD2. The firmware is wired like this:
  - **Feed:** `blescan`'s `parse_adv` hands every type-0xFF AD to `shellytime_feed()`,
    **outside** the BTHome `if (sd)` branch. Gateways carry no BTHome data, and the
    senders stay out of the `s_dev` picker list.
  - **Sync:** web/MQTT/REST `shelly_sync` only raises a flag. `shellytime_tick()` in
    the main loop applies it with the `/api/settime` trio (settimeofday + RTC +
    persisted epoch) and never touches TZ. The applied time is the median of fresh
    senders, each stamp + time since *first* heard, which removes the 0..3 s burst lag.
  - **Daily (`shelly_time_daily`) yields to NTP:** it is skipped while
    `netclock_last_sync_ms()` is under 24 h old. That was a deliberate choice
    ("NTP primary"), so flip it there if wanted.
  - **The setting lives in the old reserved `bright_min` byte** (always 0 in stored
    blobs), so no schema bump was needed. `bright_max` is still free.
  - **Don't add a NimBLE init** (the standalone scanner in
    `~/Documents/BThome_Tests/esp32_shelly_time` is for apps without BLE). blescan
    owns NimBLE here.
  - **URI handlers:** 14 of `max_uri_handlers = 16` are now used.
- **Colon blinks during OTA** (`PIN_COLON` is a dedicated GPIO, not multiplexed).
  While `s_hold_off` (OTA), the nixie ISR itself drives `PIN_COLON` from a frame
  counter (~6 Hz) — because the httpd worker is stalled in `esp_ota_write` and
  can't. `nixie_hold_off(true)` therefore leaves the colon to the ISR (don't make
  it force `PIN_COLON=0` again); `nixie_set_colon` still early-returns while held.
- **Interlude schedule is configurable** (`interlude_for_second` in `main.c`):
  temp1/temp2/date each own a `[sec, sec+len)` window (mod 60), precedence
  temp0 > temp1 > date. `show_date` renders `DDMM : DoW` — day+month on the left
  four tubes, then the colon (fixed at tubes 3|4), a blank, and the ISO weekday
  digit on tube 5. (DoW can't sit right of a colon-separated `DD:MM` because the
  colon is hardware-fixed between tubes 3|4, so the date runs together as DDMM.)
  Windows shouldn't overlap.
- **Sun mode has a twilight offset** (`sun_offset_min`, minutes): shifts the
  sunset/sunrise threshold in `settings_sun_is_night` (~0.2 deg/min approx), +ve
  keeps the display bright longer into dusk/dawn. It lives in the byte that used
  to be `auto_brightness` (dead) — repurposed in place, **same struct size so no
  schema bump / settings reset**; existing stored value (0) = 0 offset.
- **Buttons sit on tube slots 1..5** (slot 0 has no button): 1=bright+, 2=bright−,
  3=show-temp, 4=anti-poison, 5=provision (short=show IP, ~3s=provision,
  ~10s=factory reset). Snooze is a dedicated GPIO.
- **Settings schema:** bump `SETTINGS_VERSION` in `settings.h` when you change
  `clock_settings_t`; a version/size mismatch resets to defaults (creds are in a
  separate NVS namespace, so they survive). (v2→v3 added `shelly_mac`; v3→v4
  added `temp_offset` — a per-slot calibration applied in `temps_get` on
  read-out, so it shifts display + MQTT/REST alike but leaves the raw DS picker
  list untouched; v4→v5 added `night_mode`/`lat_e2`/`lon_e2` for sun/lux dimming;
  v5→v6 added `temp_sec`/`temp_len`/`date_*` (configurable interlude schedule) +
  `tz` (POSIX timezone).)
- **Wall-clock epoch is persisted OUTSIDE the settings blob** (`settings_persist_epoch`
  /`settings_load_epoch`, NVS key `"epoch"` in the `"clock"` namespace) so a
  schema bump never wipes it. `main.c` writes it every ~3h (and on every time-set
  /SNTP sync); at boot, if the RTC is dead/invalid it reseeds from this so the
  clock shows an approximate time instead of 1970.
- **The RTC chip holds UTC, not local time (don't regress).** Always go through
  `rtc_get_epoch`/`rtc_set_epoch` (UTC epoch in/out; `days_from_civil` since
  newlib has no `timegm`). It used to hold local time, which left the clock an
  hour off after an offline reboot across a DST change (or a TZ edit). Older
  RTC contents are detected by the NVS marker `"rtc_utc"` (`clock` namespace,
  outside the blob): absent = legacy local, read once via `mktime` + current TZ
  and rewritten as UTC. `rtc_set_epoch` sets the marker; `netcfg_factory_reset`
  re-sets it after the NVS wipe (the chip still holds UTC).
- **Cross-task actions:** web (httpd) and MQTT call `alarm_snooze`/`alarm_dismiss`
  /`antipoison_trigger` while the main loop ticks those modules. `alarm.c`
  guards its state transitions with a mutex (created in `alarm_init`, which is
  therefore called *before* the network surfaces start); `antipoison_trigger`
  only raises a flag that `antipoison_tick` consumes. Keep new cross-task
  actions on one of those two patterns.
- **Uptime timestamps are `int64_t` ms** (`esp_timer_get_time()/1000`) everywhere
  — no 32-bit millis (blescan used to, and wrapped every 49.7 days).
- **TZ is owned by `main.c`, not `netclock`** (was hardcoded in both). Boot applies
  `setenv("TZ", cfg.tz)` before the RTC seed and re-applies on a settings change;
  `netclock_start` must NOT `setenv` TZ (that clobbered a custom zone). Set it via
  the `tz` control key (web/MQTT) or `/api/settime?...&tz=`.
- **`settings.h` is included almost everywhere — keep it lean.** Don't add
  `<time.h>` (or other system headers) to it: doing so pulled `sys/types.h`'s
  `dev_t` into `blescan.c` and collided with its local `dev_t` typedef. That's
  why `settings_*` sun helpers take the epoch as `int64_t`, not `time_t`, and
  the `<time.h>`/`gmtime_r` live in `settings.c` only. (Same "don't shadow IDF/
  system names" lesson as `rtc_*`.)
- **BLE/NimBLE config (burned us):** BT is on for `blescan` (`sdkconfig.defaults`).
  Two non-obvious pins: (1) do **not** trim NimBLE to observer-only roles — that
  leaves `ble_hs_resolv` referencing `ble_sm_alg_encrypt` which then isn't
  compiled → link error; keep the default host roles, the real saver is
  `BTDM_CTRL_MODE_BLE_ONLY` (no Classic). (2) BT controller + WiFi/BT
  coexistence overflow IRAM alongside the gptimer-in-IRAM flags, so
  `CONFIG_ESP_WIFI_IRAM_OPT`/`_RX_IRAM_OPT` are **off** to reclaim it. After
  editing `sdkconfig.defaults`, delete `sdkconfig.esp32dev`+`sdkconfig` and
  `rm -rf .pio/build/esp32dev` so PlatformIO regenerates from defaults.

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

Also done: **Shelly BLU H&T over BLE** — `blescan` (NimBLE passive BTHome v2 scan)
+ `TEMP_SRC_SHELLY` source + `shelly_mac` per slot + live web picker
(`GET /api/blescan`) + HA select/MAC entities; verified on hardware (a BLU H&T fed
`temp1` over BLE while WiFi/MQTT stayed up). Per-slot **temp calibration offset**
(`temp_offset`, applied in `temps_get`). **Three-mode night dimming** — fixed /
sunrise-sunset (offline solar calc) / BH1750 ambient lux (the `bh1750` component;
UI offers `lux` only when the sensor is detected). *Sun mode built but not yet
hardware-verified in the field; BH1750 flow untested without a physical sensor.*

Also done (this batch): REST GET aliases (`/api/set`,`/api/action`), configurable
interlude schedule + a **date** interlude, OTA colon blink, colon second-alignment,
settable POSIX `tz`, **set-clock-from-phone** (`/api/settime` on both the web UI and
the SoftAP portal), and time persistence to NVS.

Remaining ideas are optional and user-driven: encrypted-BTHome (AES-CCM) support
for Shelly BLU, a twilight offset for sun mode (dim N min after sunset). Confirm
scope with the user before building.
