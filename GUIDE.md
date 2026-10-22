# Nixie Clock — User Guide / Uživatelská příručka

A six-tube ESP32 nixie clock with WiFi, Home Assistant, a built-in web page,
alarm, and temperature display.

**🇬🇧 [English](#english) · 🇨🇿 [Čeština](#čeština)**

> Each language has two parts: **For everyone** (daily use) and **Power users**
> (network, MQTT, REST, building the firmware).
>
> Každý jazyk má dvě části: **Pro každého** (běžné používání) a **Pro pokročilé**
> (síť, MQTT, REST, sestavení firmwaru).

---

# English

## What it does

It shows the time on six nixie tubes and keeps it accurate over WiFi (internet
time sync). Even with no network it keeps running and keeps time thanks to its
built-in battery-backed clock chip. On top of the time it can:

- **Dim itself at night** on a schedule.
- **Show the temperature** for a few seconds at the end of each minute — from a
  plug-in sensor or pushed in from your smart home.
- **Wake you up** with an alarm (per-day-of-week, snooze, choice of melodies).
- Be controlled from your **phone or computer** (a web page) and from **Home
  Assistant**.
- **Protect the tubes** from "cathode poisoning" by cycling all digits
  periodically (keeps them bright for years).

## For everyone

### First start

1. Power the clock. The tubes do a "slot-machine" animation until it knows the
   time.
2. If it has never been set up, it starts its own WiFi network for setup — see
   **Connect it to your WiFi** below.
3. Once on your WiFi and time-synced, it just shows the time.

### Connect it to your WiFi

The clock can be set up entirely from your phone, no cable needed:

1. Put it in setup mode: **hold button 5** (the rightmost / provision button)
   for about **3 seconds** until it switches to setup.
2. The tubes show an **8-digit PIN** in two halves — the lit colon means you're
   looking at the first half, then it shows the second half.
3. On your phone, join the WiFi network called **`NixieClock-XXXXXX`**. When it
   asks for a password, type the 8-digit PIN from the tubes. (This way only
   someone standing at the clock can set it up.)
4. A setup page opens automatically. Pick your home WiFi, type its password,
   press save.
5. The clock reboots and connects to your network. Done.

> Lost track of the clock's address later? **Tap button 5 once** and it spells
> out its IP address on the tubes.

### The buttons

There are five buttons across the front (under tube positions 1–5) plus a
dedicated **snooze** button.

| Button | Tap | Hold |
|--------|-----|------|
| **1** | Brightness up | — |
| **2** | Brightness down | — |
| **3** | Show temperature now | — |
| **4** | Run anti-poison cycle now | — |
| **5** | Show IP address | ~3 s: WiFi setup · ~10 s: factory reset |
| **Snooze** | Snooze the ringing alarm | Dismiss the alarm (until tomorrow) |

> **Handy combo:** hold **brightness up + down together** (~2 s) to toggle the
> alarm on or off.

### The web page

Open `http://<clock-ip>/` in any browser on the same network (find the IP by
tapping button 5). From there you can set:

- Brightness, and a **night schedule** (dimmer between, e.g., 20:30 and 06:00).
- **Anti-poisoning** — the "scrub" that runs every digit on every tube so rarely
  used digits don't fade. You can set how often (minutes), how long (seconds), how
  fast (ms per step) and which **animation** to use: cascade, sync, random, reels,
  wave, or mix, which uses a different one each time. **Pause for interludes**
  starts a scheduled scrub only where it fits between the temperature/date
  readouts, so it never hides them; it waits a minute at most. The **Scrub**
  button runs one straight away, which doubles as a preview, and **Next scrub**
  shows when the next one is due.
- **Alarm** — time, which days of the week, melody, snooze length. There's a
  **Play** button to preview the selected melody, and a **Status** line showing
  whether it's armed, snoozed, or off until the next ring.
- **Temperature** — turn it on, how many decimals, and which sensor feeds each
  of the two slots.
- **Network** — change WiFi / smart-home settings later without the PIN dance.
- **Clock & time** — timezone, setting the clock from your phone, and the
  optional **Shelly BLE time**. If you have newer Shelly devices (Gen2 or later, firmware
  2.x, used as a Bluetooth gateway for Shelly BLU sensors), they broadcast the
  current time over Bluetooth. **Sync from Shelly now** takes it straight away. **Daily sync from
  Shelly** does it once a day, but only when internet time (NTP) hasn't worked for
  a day, so NTP always stays the main source. Below the buttons you can see what
  happened, and which Shellies the clock hears and how far their time is from the clock's.

Changes apply instantly and stay in sync everywhere (web page, Home Assistant,
buttons all share the same settings).

### What the colon is telling you

The colon between the digits also signals the alarm state at a glance:

| Colon | Meaning |
|-------|---------|
| Normal blink / steady | Alarm off |
| One short pip per second | Alarm on |
| Two short pips per second | Snoozed (will ring again soon) |
| Whole display flashing | Alarm ringing now |

### Temperature sensors

The clock can show up to **two** temperatures near the end of each minute. Each
slot can be fed by either:

- a **plug-in DS18B20 sensor** (a small wired probe),
- a **Shelly BLU H&T** sensor (wireless, read directly over Bluetooth — no hub,
  gateway or cloud needed), or
- a value **pushed from your smart home** (e.g. the outdoor temperature).

Open the web page — every detected DS18B20 **and** every nearby Shelly BLU sensor
is listed with its **live reading** and an **Assign** button, so you don't have to
copy any codes by hand. (Shelly BLU must be left **unencrypted** in the Shelly app,
which is the default.) Not sure which BLU is which? **Press its button** and its row
highlights in the list. Negative temperatures are shown by **blinking** the reading
(the tubes have no minus sign), and the colon acts as the decimal point. A Shelly
sensor's **battery level** appears in the web UI and Home Assistant; when it drops
**≤20 %** the clock **blinks just the colon** during that reading — a distinct cue
from the negative-temperature whole-reading blink.

### Factory reset

Hold **button 5 for ~10 seconds** (or use the button on the web page / Home
Assistant). This wipes all settings and WiFi credentials and returns the clock
to first-start setup mode. (The web/Home-Assistant button asks for confirmation
so it can't be triggered by accident.)

---

## Power users

Hardware: ESP32 (WROOM-32) on a **UCB32** controller driving a **Unidisp**
display board. RTC is a **PCF8563** at I²C `0x51` (the driver also auto-detects
DS1307/DS3231 at `0x68`). The HV supply has no dim input, so **brightness is
done in firmware** by PWM-ing each tube's anode within its multiplex slot.

Full pin map: [`components/common/include/pins.h`](components/common/include/pins.h).
Key lines: tube select `GPIO16/17/18`, BCD cathodes `GPIO23/25/26/27`, colon
`GPIO33`, buttons `GPIO14` (multiplexed, active-low) + snooze `GPIO15`, buzzer
`GPIO19`, I²C `GPIO21/22`, 1-Wire `GPIO13` (needs a **4.7 kΩ** pull-up to 3.3 V).

### Control surfaces

All three surfaces go through one shared settings store, so a change on any of
them is reflected on the others immediately.

#### Web UI + REST/JSON

Served by the clock itself at `http://<clock-ip>/`.

| Method & path | Purpose |
|---------------|---------|
| `GET /api/state` | Full device state as JSON |
| `POST /api/set?k=<key>` | Set a setting; value in the request body |
| `GET /api/set?k=<key>&v=<value>` | Same, value in the query (no body needed) |
| `POST /api/action?a=<action>` | Trigger an action; payload in body where noted |
| `GET /api/action?a=<action>&v=<value>` | Same, value in the query |
| `GET \| POST /api/settime?epoch=<unix>&tz=<POSIX>` | Set the clock (and optional TZ) from a client — no network/NTP needed |
| `GET /api/net` | Current network config |
| `POST /api/netset` | Update WiFi/MQTT credentials |
| `POST /api/ota` | Firmware update: raw `firmware.bin` in the body |
| `GET /api/log` | Recent device log (RAM buffer, plain text) |
| `GET /api/blescan` | Nearby Shelly BLU (BTHome) sensors as JSON |
| `GET /api/shellytime` | Shelly BLE time status: last sync result, NTP / daily state, UTC offset Shellies vs. clock, every time-beacon sender with RSSI, age and `delta_ms` (its time minus the clock's) |

**Settings keys** (`/api/set?k=…`, value in body):

```
brightness            0..255
night_enabled         ON / OFF      (master toggle for dimming at night)
night_mode            fixed / sun / lux   (lux only if a BH1750 is present)
night_brightness      0..255        (level used when it's "night")
night_start           HH:MM         (fixed mode: window start)
night_end             HH:MM         (fixed mode: window end)
latitude              e.g. 50.08    (sun mode: degrees N+, offline sunrise calc)
longitude             e.g. 14.44    (sun mode: degrees E+)
sun_offset            -120..120 min (sun mode twilight; +ve = dim later after
                                     sunset & brighten earlier before sunrise,
                                     i.e. stay bright through twilight; -ve = the
                                     opposite. Approximate minutes, ~0.2°/min.)
blink_colon           ON / OFF
h24                   ON / OFF      (off = 12-hour, blanks leading zero)
ap_enabled            ON / OFF      (anti-poison enabled)
ap_period             1..1440       (minutes between scheduled scrubs, default 10)
ap_duration           1..120        (seconds per scrub, default 12)
ap_speed              20..1000      (ms per animation step, default 120)
ap_anim               cascade / sync / random / reels / wave / mix
ap_pause              ON / OFF      (scheduled scrubs start only where they fit
                                     between the temp/date interludes; default OFF)
alarm_enabled         ON / OFF
alarm_time            HH:MM
alarm_melody          0..7          (see melody table)
alarm_snooze          minutes
alarm_dow             0..127        (bitmask: bit0=Sun … bit6=Sat; 0 = every day)
temp_enabled          ON / OFF
temp_decimals         0..2
temp1_source          none / mqtt / ds18b20 / shelly
temp2_source          none / mqtt / ds18b20 / shelly
temp1_rom             16-hex ROM    (empty string clears; used when source=ds18b20)
temp2_rom             16-hex ROM
temp1_mac             AA:BB:CC:DD:EE:FF  (empty clears; used when source=shelly)
temp2_mac             AA:BB:CC:DD:EE:FF
temp1_offset          calibration in °C, e.g. 0.5 / -1.2 (empty/0 = none), any source
temp2_offset          calibration in °C
temp1_sec / temp1_len  interlude: which second (0..59) temp1 shows + length (1..8 s)
temp2_sec / temp2_len  interlude for temp2  (default 55/2)
date_enabled          ON / OFF      (date interlude: DDMM : DoW)
date_sec / date_len   date interlude second + length (default 57/2)
tz                    POSIX timezone, e.g. CET-1CEST,M3.5.0,M10.5.0/3
shelly_time_daily     ON / OFF      (daily clock sync from the Shelly BLE time
                                     beacon: ~1 min after boot, then every 24 h,
                                     skipped while NTP synced within 24 h)
```

**Actions** (`/api/action?a=…`):

```
shelly_sync           set the clock from the Shelly BLE time beacon now (waits
                      up to 25 s for one; result in /api/shellytime and the log)
antipoison_now        run the anti-poison cycle now
alarm_snooze          snooze a ringing alarm
alarm_dismiss         dismiss the alarm until tomorrow
alarm_test            preview the currently selected alarm melody once
mqtt_test             force an MQTT reconnect
temp1_input           push an MQTT-style value into slot 1 (value in body)
temp2_input           push a value into slot 2
factory_reset         wipe NVS + reboot — body MUST be the token "RESET"
```

`/api/state` returns, among others, a `ds` array listing every DS18B20 on the
bus with its live reading and assigned slot:

```json
"ds":[{"rom":"EE0623C3555D5028","t":"23.45","slot":1},
      {"rom":"9C0623C3452EE228","t":"21.10","slot":0}]
```

#### MQTT + Home Assistant

On connect the clock publishes **Home Assistant MQTT discovery** and appears as
a single *Nixie Clock* device (`nixie-<mac>`). Topic layout:

```
nixieclock/<node>/state            retained-ish JSON state (published on change)
nixieclock/<node>/availability     "online" / "offline" (LWT)
nixieclock/<node>/cmd/<key>        command topics — same keys as /api/set
homeassistant/.../config           discovery configs (retained)
```

Each discovered DS18B20 also gets its **own** temperature entity, regardless of
whether it's assigned to a display slot — so a sensor that only feeds Home
Assistant is still visible there. Its value comes from the `ds` object in the
state payload via a templated topic.

> Note: removing a DS18B20 leaves its retained discovery config behind; the
> entity goes *unavailable* rather than being auto-deleted.

Destructive commands are guarded: `factory_reset` requires the literal token
`RESET` in the payload, so a stray or retained message can't wipe the device.

Shelly BLE time shows up as a **Daily Shelly time sync** switch
(`cmd/shelly_time_daily`) and a **Sync time from Shelly** button
(`cmd/shelly_sync`, any payload).

### Credentials & provisioning

Credentials live in NVS. `components/common/include/secrets.h` (gitignored) is a
**first-boot fallback only** — once a WiFi SSID is stored in NVS the device
trusts NVS exclusively (an empty MQTT host means *no MQTT*, the client goes
idle). A factory reset erases NVS and forces SoftAP setup on the next boot,
regardless of `secrets.h`.

To seed defaults at compile time:

```sh
cp components/common/include/secrets.example.h components/common/include/secrets.h
# edit WIFI_SSID / WIFI_PASS / MQTT_*
```

### Build, flash, monitor

Requires [PlatformIO](https://platformio.org/install) (ESP-IDF 5.5). It fetches
the toolchain and managed components (`espressif/onewire_bus`, `espressif/ds18b20`)
on first build.

```sh
pio run                 # build
pio run -t upload       # build + flash over USB (serial / FTDI)
pio device monitor      # serial console @ 115200
```

> **No auto-reset on the board.** Before uploading, hand-enter download mode:
> **hold IO0, tap EN, release IO0.** Run only one upload at a time.

After adding/removing a component or editing a `REQUIRES`/`idf_component.yml`,
clear the stale CMake cache: `rm -rf .pio/build/esp32dev` then rebuild.

Flash layout: 4 MB, custom `partitions.csv` — dual-OTA (two ~1.875 MB app slots +
`otadata` + NVS + SPIFFS). App is ~67 % of a slot.

**OTA updates:** after the first serial flash, push new builds over WiFi from the
web UI's **Firmware** section (pick `.pio/build/esp32dev/firmware.bin` → Upload). It
writes the spare slot and reboots into it; the **Running** line shows the version and
active slot. A bad/interrupted upload leaves the current firmware bootable, and NVS
(settings + credentials) is never touched.

### Melody table (`alarm_melody`)

| # | Name | # | Name |
|---|------|---|------|
| 0 | Beep | 4 | Siren |
| 1 | Reveille | 5 | Nokia |
| 2 | Westminster | 6 | Axel F |
| 3 | Scale | 7 | Triad |

Melodies are RTTTL (Nokia ringtone format); the table lives in
[`components/buzzer/buzzer.c`](components/buzzer/buzzer.c).

### Display quirks (by design)

- **No decimal points** on the tubes — the colon between MM:SS is the only
  separator and doubles as the decimal point in temperature mode.
- **No minus glyph** — negative temperatures blink the whole reading.
- Brightness is multiplex duty-cycling; the cathode-before-anode write order and
  dead-time are the ghosting fix — don't reorder them.

---

# Čeština

## Co hodiny umí

Ukazují čas na šesti nixie tubicích a udržují ho přesný přes WiFi (synchronizace
času z internetu). I bez sítě běží dál a drží čas díky vestavěnému hodinovému
čipu se zálohovací baterií. Navíc umí:

- **Ztlumit se v noci** podle časového plánu.
- **Zobrazit teplotu** na pár vteřin na konci každé minuty — z připojeného
  čidla nebo poslanou z chytré domácnosti.
- **Vzbudit budíkem** (podle dne v týdnu, odložení, výběr melodií).
- Ovládat se z **telefonu nebo počítače** (webová stránka) a z **Home Assistanta**.
- **Chránit tubice** před „otravou katody" pravidelným protočením všech číslic
  (udrží je jasné po léta).

## Pro každého

### První spuštění

1. Připojte napájení. Tubice dělají „výherní automat", dokud hodiny neznají čas.
2. Pokud ještě nebyly nastaveny, spustí vlastní WiFi síť pro nastavení — viz
   **Připojení k WiFi** níže.
3. Jakmile jsou na vaší WiFi a mají synchronizovaný čas, jen ukazují čas.

### Připojení k WiFi

Hodiny se dají nastavit celé z telefonu, bez kabelu:

1. Přepněte je do režimu nastavení: **podržte tlačítko 5** (nejpravější /
   provisioning) asi **3 vteřiny**, dokud nepřejde do nastavení.
2. Tubice ukáží **8místný PIN** ve dvou polovinách — svítící dvojtečka znamená,
   že vidíte první polovinu, pak se ukáže druhá.
3. V telefonu se připojte k WiFi síti **`NixieClock-XXXXXX`**. Až se zeptá na
   heslo, zadejte 8místný PIN z tubic. (Takhle hodiny nastaví jen ten, kdo u
   nich stojí.)
4. Automaticky se otevře stránka nastavení. Vyberte svou domácí WiFi, zadejte
   heslo, uložte.
5. Hodiny se restartují a připojí k vaší síti. Hotovo.

> Zapomněli jste později adresu hodin? **Ťukněte jednou na tlačítko 5** a hodiny
> vypíší svou IP adresu na tubicích.

### Tlačítka

Vepředu je pět tlačítek (pod pozicemi tubic 1–5) plus vyhrazené tlačítko
**snooze** (odložení).

| Tlačítko | Ťuknutí | Podržení |
|----------|---------|----------|
| **1** | Jas nahoru | — |
| **2** | Jas dolů | — |
| **3** | Ukázat teplotu hned | — |
| **4** | Spustit anti-poison cyklus | — |
| **5** | Ukázat IP adresu | ~3 s: nastavení WiFi · ~10 s: tovární reset |
| **Snooze** | Odložit zvonící budík | Vypnout budík (do zítřka) |

> **Šikovná kombinace:** podržte **jas nahoru + dolů zároveň** (~2 s) pro
> zapnutí/vypnutí budíku.

### Webová stránka

Otevřete `http://<ip-hodin>/` v jakémkoli prohlížeči ve stejné síti (IP zjistíte
ťuknutím na tlačítko 5). Můžete tam nastavit:

- Jas a **noční plán** (ztlumení např. mezi 20:30 a 06:00).
- **Anti-poisoning** — „čištění“, které projede všechny číslice na všech
  tubicích, aby málo používané číslice nevybledly. Nastavíte, jak často (minuty),
  jak dlouho (sekundy), jak rychle (ms na krok) a jakou **animací**: cascade,
  sync, random, reels, wave, nebo mix, která pokaždé použije jinou.
  **Pause for interludes** spustí plánované čištění jen tam, kde se vejde mezi
  zobrazení teploty a data, takže je nikdy nepřekryje; čeká nanejvýš minutu.
  Tlačítko **Scrub** spustí čištění hned (poslouží i jako náhled) a **Next scrub**
  ukazuje, kdy proběhne další.
- **Budík** — čas, dny v týdnu, melodii, délku odložení. Je tu tlačítko **Play**
  pro přehrání vybrané melodie a řádek **Status**, který ukazuje, jestli je budík
  natažený, odložený, nebo vypnutý do dalšího zvonění.
- **Teplotu** — zapnutí, počet desetinných míst a které čidlo napájí každý ze
  dvou slotů.
- **Síť** — změnit WiFi / chytrou domácnost později bez tancování s PINem.
- **Hodiny a čas** — časová zóna, nastavení času z telefonu a volitelný
  **čas ze Shelly přes BLE**. Novější zařízení Shelly (Gen2 a novější, firmware
  2.x, používaná jako Bluetooth brána pro čidla Shelly BLU) vysílají aktuální čas
  přes Bluetooth. **Sync from Shelly now** ho převezme hned. **Daily sync from
  Shelly** to dělá jednou denně, ale jen když čas z internetu (NTP) den nefungoval,
  takže NTP zůstává hlavním zdrojem. Pod tlačítky je vidět, jak synchronizace
  dopadla, které Shelly hodiny slyší a o kolik se jejich čas liší od času hodin.

Změny se projeví okamžitě a zůstávají všude synchronizované (web, Home Assistant
i tlačítka sdílí stejné nastavení).

### Co vám napovídá dvojtečka

Dvojtečka mezi číslicemi také naznačuje stav budíku na první pohled:

| Dvojtečka | Význam |
|-----------|--------|
| Normální blikání / svítí | Budík vypnutý |
| Jedno krátké bliknutí za vteřinu | Budík zapnutý |
| Dvě krátká bliknutí za vteřinu | Odložený (brzy zase zazvoní) |
| Bliká celý displej | Budík právě zvoní |

### Teplotní čidla

Hodiny umí ukázat až **dvě** teploty ke konci každé minuty. Každý slot může být
napájen buď:

- **připojeným čidlem DS18B20** (malá sonda na drátku),
- čidlem **Shelly BLU H&T** (bezdrátové, čtené přímo přes Bluetooth — bez hubu,
  brány i cloudu), nebo
- hodnotou **poslanou z chytré domácnosti** (např. venkovní teplota).

Otevřete web — každé nalezené čidlo DS18B20 **i** každé blízké čidlo Shelly BLU je
v seznamu s **živým náměrem** a tlačítkem **Assign** (přiřadit), takže nemusíte
ručně opisovat žádné kódy. (Shelly BLU musí být v aplikaci Shelly **nešifrované**,
což je výchozí stav.) Nevíte, které čidlo je které? **Stiskněte jeho tlačítko** a
jeho řádek se v seznamu zvýrazní. Záporné teploty se ukazují **blikáním** náměru (tubice
nemají znaménko mínus) a dvojtečka slouží jako desetinná čárka. **Stav baterie**
čidla Shelly je vidět ve webu i v Home Assistantu; když klesne na **≤20 %**, hodiny
při daném náměru **blikají jen dvojtečkou** — odlišně od blikání celého náměru u
záporných teplot.

### Tovární reset

Podržte **tlačítko 5 asi 10 vteřin** (nebo použijte tlačítko na webu / v Home
Assistantovi). Smaže všechna nastavení i WiFi údaje a vrátí hodiny do režimu
prvního nastavení. (Tlačítko na webu / v HA vyžaduje potvrzení, aby nešlo
spustit omylem.)

---

## Pro pokročilé

Hardware: ESP32 (WROOM-32) na řadiči **UCB32** ovládajícím zobrazovací desku
**Unidisp**. RTC je **PCF8563** na I²C `0x51` (ovladač auto-detekuje i
DS1307/DS3231 na `0x68`). Vysokonapěťový zdroj nemá vstup pro stmívání, takže se
**jas dělá ve firmwaru** PWM modulací anody každé tubice v jejím multiplexovém
okně.

Kompletní mapa pinů: [`components/common/include/pins.h`](components/common/include/pins.h).
Hlavní: výběr tubice `GPIO16/17/18`, BCD katody `GPIO23/25/26/27`, dvojtečka
`GPIO33`, tlačítka `GPIO14` (multiplexovaná, aktivní v nule) + snooze `GPIO15`,
bzučák `GPIO19`, I²C `GPIO21/22`, 1-Wire `GPIO13` (potřebuje **4,7 kΩ** pull-up
na 3,3 V).

### Ovládací rozhraní

Všechna tři rozhraní jdou přes jedno sdílené úložiště nastavení, takže změna na
kterémkoli se okamžitě projeví na ostatních.

#### Webové UI + REST/JSON

Hostují samy hodiny na `http://<ip-hodin>/`.

| Metoda a cesta | Účel |
|----------------|------|
| `GET /api/state` | Kompletní stav zařízení jako JSON |
| `POST /api/set?k=<klíč>` | Nastavit hodnotu; hodnota v těle požadavku |
| `GET /api/set?k=<klíč>&v=<hodnota>` | Totéž, hodnota v URL (bez těla) |
| `POST /api/action?a=<akce>` | Spustit akci; data v těle kde je uvedeno |
| `GET /api/action?a=<akce>&v=<hodnota>` | Totéž, hodnota v URL |
| `GET \| POST /api/settime?epoch=<unix>&tz=<POSIX>` | Nastavit čas (a případně TZ) z klienta — bez sítě/NTP |
| `GET /api/net` | Aktuální síťová konfigurace |
| `POST /api/netset` | Změnit WiFi/MQTT údaje |
| `POST /api/ota` | Aktualizace firmwaru: `firmware.bin` v těle |
| `GET /api/log` | Nedávný log zařízení (RAM buffer, text) |
| `GET /api/blescan` | Blízká čidla Shelly BLU (BTHome) jako JSON |
| `GET /api/shellytime` | Stav času ze Shelly přes BLE: výsledek poslední synchronizace, stav NTP / denní synchronizace, posun od UTC u Shelly a u hodin, všichni vysílači časového majáku s RSSI, stářím a `delta_ms` (jejich čas mínus čas hodin) |

**Klíče nastavení** (`/api/set?k=…`, hodnota v těle):

```
brightness            0..255
night_enabled         ON / OFF      (hlavní vypínač nočního ztlumení)
night_mode            fixed / sun / lux   (lux jen když je přítomen BH1750)
night_brightness      0..255        (jas použitý v „noci“)
night_start           HH:MM         (režim fixed: začátek okna)
night_end             HH:MM         (režim fixed: konec okna)
latitude              např. 50.08   (režim sun: stupně S+, offline výpočet)
longitude             např. 14.44   (režim sun: stupně V+)
sun_offset            -120..120 min (soumrak; kladné = ztlumí později po západu
                                     a rozsvítí dříve před východem, tj. déle
                                     svítí do šera; záporné = opačně. Přibližné
                                     minuty, ~0.2°/min.)
blink_colon           ON / OFF
h24                   ON / OFF      (off = 12hodinový, skryje úvodní nulu)
ap_enabled            ON / OFF      (anti-poison zapnut)
ap_period             1..1440       (minuty mezi plánovanými čištěními, výchozí 10)
ap_duration           1..120        (sekundy na jedno čištění, výchozí 12)
ap_speed              20..1000      (ms na krok animace, výchozí 120)
ap_anim               cascade / sync / random / reels / wave / mix
ap_pause              ON / OFF      (plánované čištění začne jen tam, kde se vejde
                                     mezi vsuvky teploty/data; výchozí OFF)
alarm_enabled         ON / OFF
alarm_time            HH:MM
alarm_melody          0..7          (viz tabulka melodií)
alarm_snooze          minuty
alarm_dow             0..127        (bitmaska: bit0=Ne … bit6=So; 0 = každý den)
temp_enabled          ON / OFF
temp_decimals         0..2
temp1_source          none / mqtt / ds18b20 / shelly
temp2_source          none / mqtt / ds18b20 / shelly
temp1_rom             16 hex znaků ROM   (prázdný řetězec smaže; když source=ds18b20)
temp2_rom             16 hex znaků ROM
temp1_mac             AA:BB:CC:DD:EE:FF  (prázdný smaže; když source=shelly)
temp2_mac             AA:BB:CC:DD:EE:FF
temp1_offset          kalibrace ve °C, např. 0.5 / -1.2 (prázdné/0 = žádná), pro každý zdroj
temp2_offset          kalibrace ve °C
temp1_sec / temp1_len  vsuvka: v kterou sekundu (0..59) se temp1 zobrazí + délka (1..8 s)
temp2_sec / temp2_len  vsuvka pro temp2  (výchozí 55/2)
date_enabled          ON / OFF      (vsuvka s datem: DDMM : DoW)
date_sec / date_len   sekunda + délka vsuvky s datem (výchozí 57/2)
tz                    POSIX časová zóna, např. CET-1CEST,M3.5.0,M10.5.0/3
shelly_time_daily     ON / OFF      (denní synchronizace času z BLE majáku Shelly:
                                     ~1 min po startu, pak každých 24 h; vynechá
                                     se, pokud NTP synchronizoval během 24 h)
```

**Akce** (`/api/action?a=…`):

```
shelly_sync           nastavit čas hned z BLE majáku Shelly (čeká na něj až
                      25 s; výsledek v /api/shellytime a v logu)
antipoison_now        spustit anti-poison cyklus hned
alarm_snooze          odložit zvonící budík
alarm_dismiss         vypnout budík do zítřka
alarm_test            přehrát jednou aktuálně vybranou melodii budíku
mqtt_test             vynutit znovupřipojení MQTT
temp1_input           poslat MQTT hodnotu do slotu 1 (hodnota v těle)
temp2_input           poslat hodnotu do slotu 2
factory_reset         smazat NVS + restart — tělo MUSÍ být token "RESET"
```

`/api/state` mimo jiné vrací pole `ds` se všemi čidly DS18B20 na sběrnici, jejich
živým náměrem a přiřazeným slotem:

```json
"ds":[{"rom":"EE0623C3555D5028","t":"23.45","slot":1},
      {"rom":"9C0623C3452EE228","t":"21.10","slot":0}]
```

#### MQTT + Home Assistant

Po připojení hodiny publikují **Home Assistant MQTT discovery** a objeví se jako
jediné zařízení *Nixie Clock* (`nixie-<mac>`). Struktura topiců:

```
nixieclock/<node>/state            JSON stav (publikován při změně)
nixieclock/<node>/availability     "online" / "offline" (LWT)
nixieclock/<node>/cmd/<klíč>       příkazové topiky — stejné klíče jako /api/set
homeassistant/.../config           discovery konfigurace (retained)
```

Každé nalezené čidlo DS18B20 dostane i **vlastní** teplotní entitu, bez ohledu
na to, zda je přiřazené k zobrazovacímu slotu — takže čidlo, které jen napájí
Home Assistant, je tam stejně vidět. Jeho hodnota se bere z objektu `ds` ve
stavovém payloadu přes šablonovaný topic.

> Pozn.: odebrání čidla DS18B20 zanechá jeho retained discovery konfiguraci;
> entita přejde do stavu *nedostupná* místo automatického smazání.

Destruktivní příkazy jsou pojištěné: `factory_reset` vyžaduje v payloadu doslovný
token `RESET`, aby zatoulaná nebo retained zpráva nemohla zařízení smazat.

Čas ze Shelly přes BLE se objeví jako přepínač **Daily Shelly time sync**
(`cmd/shelly_time_daily`) a tlačítko **Sync time from Shelly**
(`cmd/shelly_sync`, libovolný payload).

### Údaje a provisioning

Přihlašovací údaje jsou v NVS. `components/common/include/secrets.h` (gitignored)
je **pouze záloha pro první boot** — jakmile je v NVS uložené WiFi SSID, zařízení
důvěřuje výhradně NVS (prázdný MQTT host znamená *žádné MQTT*, klient se uspí).
Tovární reset smaže NVS a při dalším bootu vynutí SoftAP nastavení bez ohledu na
`secrets.h`.

Nasazení výchozích hodnot při kompilaci:

```sh
cp components/common/include/secrets.example.h components/common/include/secrets.h
# uprav WIFI_SSID / WIFI_PASS / MQTT_*
```

### Sestavení, flashování, monitor

Vyžaduje [PlatformIO](https://platformio.org/install) (ESP-IDF 5.5). Při prvním
buildu si stáhne toolchain a managed komponenty (`espressif/onewire_bus`,
`espressif/ds18b20`).

```sh
pio run                 # sestavit
pio run -t upload       # sestavit + flashnout přes USB (sériový port / FTDI)
pio device monitor      # sériová konzole @ 115200
```

> **Deska nemá auto-reset.** Před nahráním ručně vstupte do download módu:
> **podržte IO0, ťukněte EN, pusťte IO0.** Spouštějte vždy jen jeden upload.

Po přidání/odebrání komponenty nebo úpravě `REQUIRES`/`idf_component.yml` smažte
starou CMake cache: `rm -rf .pio/build/esp32dev` a sestavte znovu.

Rozložení flash: 4 MB, vlastní `partitions.csv` — dual-OTA (dva ~1,875 MB app
sloty + `otadata` + NVS + SPIFFS). App zabírá ~67 % slotu.

**OTA aktualizace:** po prvním flashnutí přes kabel nahrávejte nové buildy přes WiFi
ze sekce **Firmware** ve webovém UI (vyberte `.pio/build/esp32dev/firmware.bin` →
Upload). Zapíše se do volného slotu a hodiny se do něj restartují; řádek **Running**
ukazuje verzi a aktivní slot. Neúspěšné/přerušené nahrání ponechá stávající firmware
funkční a NVS (nastavení + údaje) zůstává nedotčené.

### Tabulka melodií (`alarm_melody`)

| # | Název | # | Název |
|---|-------|---|-------|
| 0 | Beep | 4 | Siren |
| 1 | Reveille | 5 | Nokia |
| 2 | Westminster | 6 | Axel F |
| 3 | Scale | 7 | Triad |

Melodie jsou v RTTTL (formát Nokia vyzvánění); tabulka je v
[`components/buzzer/buzzer.c`](components/buzzer/buzzer.c).

### Zvláštnosti displeje (záměrné)

- **Žádné desetinné tečky** na tubicích — dvojtečka mezi MM:SS je jediný
  oddělovač a v režimu teploty slouží jako desetinná čárka.
- **Žádný znak mínus** — záporné teploty blikají celým náměrem.
- Jas je řízen střídou multiplexu; pořadí zápisu katoda-před-anodou a dead-time
  jsou oprava ghostingu — nepřehazovat.
