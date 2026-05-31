// NixieClock firmware — entry point.
//
// Time source of truth = the ESP's own system clock (time()/localtime_r). Two
// things feed it, so the clock is robust whether or not either is present:
//   - SNTP over WiFi sets the system clock (and mirrors it into the RTC).
//   - At boot, if the RTC holds a valid time, we seed the system clock from it
//     so the correct time shows immediately, before WiFi/SNTP comes up.
//
// Display behaviour is driven by the persisted settings store:
//   - brightness = manual level, or the night level inside the night window,
//     or (future) a BH1750 reading when auto-brightness is on;
//   - colon blink / 12-24h format are settings too;
//   - anti-poison period/duration come from settings;
//   - near the end of each minute the display briefly shows temperature slots.
// When the system clock has no valid time yet (cold boot, no RTC, pre-sync) we
// run the "slot-machine" animation so you can see the clock is alive.
#include "nixie.h"
#include "rtc_clock.h"
#include "i2cbus.h"
#include "netclock.h"
#include "antipoison.h"
#include "settings.h"
#include "mqttctrl.h"
#include "temps.h"
#include "buttons.h"
#include "alarm.h"
#include "buzzer.h"
#include "webui.h"
#include "provision.h"

#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include <stdio.h>

static const char *TAG = "main";

#define LOOP_MS         50
#define MIN_VALID_YEAR  2023   // tm_year+1900 below this => time not set yet
#define NO_SENSOR       (-1)   // BH1750 not present/enabled yet
#define BRIGHT_STEP     25     // button brightness increment
#define SHOW_TEMP_MS    4000   // "show temp now" duration (2s per slot)
#define CONFIRM_MS      1000   // dismiss-confirm blink window (~2 blinks @250ms)
#define IP_OCTET_MS     1000   // per-octet dwell when showing the IP
#define IP_GAP_MS        250   // brief blank between octets

// Set by the SHOW_TEMP button: while now_ms < this, the display forces the
// temperature slots regardless of the normal :52-:55 schedule.
static volatile int64_t s_force_temp_until;
// Set on alarm dismiss: blink the time a couple times to confirm the long-press.
static volatile int64_t s_confirm_until;
// Set by the SHOW_IP button: walk the 4 IP octets across the tubes until this.
static volatile int64_t s_show_ip_until;
static uint8_t s_ip_octet[4];   // captured at press time

// Push the anti-poison period/duration from settings into the routine.
static void apply_antipoison(const clock_settings_t *c)
{
    antipoison_set_period_s(c->ap_enabled ? c->ap_period_s : 0);
    antipoison_set_duration_ms(c->ap_duration_ms);
}

// Fill the framebuffer with HH MM SS, left to right (idx 0 = hours-tens).
static void show_time(const struct tm *lt, bool h24)
{
    int hour = lt->tm_hour;
    bool blank_h_tens = false;
    if (!h24) {
        hour %= 12;
        if (hour == 0) hour = 12;
        blank_h_tens = (hour < 10);   // 12-hour: no leading zero on hours
    }

    uint8_t d[NIXIE_NUM_TUBES] = {
        (uint8_t)(hour / 10), (uint8_t)(hour % 10),
        (uint8_t)(lt->tm_min / 10), (uint8_t)(lt->tm_min % 10),
        (uint8_t)(lt->tm_sec / 10), (uint8_t)(lt->tm_sec % 10),
    };
    if (blank_h_tens) d[0] = NIXIE_BLANK;
    nixie_set_digits(d);
}

// Show a temperature (centi-degrees C) right-aligned on the tubes. These tubes
// have NO per-digit decimal points, so the colon (the only separator, between
// the MM:SS pair = tubes 3|4) is lit steady to act as the decimal point: a
// 2-decimal reading 23.45 lands as 2 3 : 4 5. This function drives the colon
// itself. No minus glyph exists, so negative values blink the whole reading
// (digits + colon) at ~2 Hz.
static void show_temp(int16_t centi, uint8_t decimals, int64_t now_ms)
{
    bool neg = centi < 0;
    int v = neg ? -centi : centi;          // magnitude, centi-degrees
    if (v > 9999) v = 9999;                // clamp to what 4 tubes can show

    uint8_t d[NIXIE_NUM_TUBES] = {
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK,
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK
    };

    // Negative + blink "off" phase: show nothing this frame (colon off too).
    if (neg && ((now_ms / 250) % 2 != 0)) {
        nixie_set_digits(d);
        nixie_set_colon(false);
        return;
    }

    bool colon = false;
    if (decimals >= 2) {
        int integer = v / 100, frac = v % 100;
        d[5] = frac % 10;
        d[4] = (frac / 10) % 10;
        d[3] = integer % 10;
        if (integer >= 10) d[2] = (integer / 10) % 10;
        colon = true;                      // colon sits between tubes 3|4 = the point
    } else if (decimals == 1) {
        int t = (v + 5) / 10;              // round to tenths
        int integer = t / 10, tenth = t % 10;
        d[4] = tenth;
        d[3] = integer % 10;
        if (integer >= 10) d[2] = (integer / 10) % 10;
        colon = true;
    } else {
        int deg = (v + 50) / 100;          // round to whole degrees
        d[5] = deg % 10;
        if (deg >= 10) d[4] = (deg / 10) % 10;
        colon = false;                     // whole degrees: no decimal point
    }
    nixie_set_digits(d);
    nixie_set_colon(colon);
}

// "Alive but unset": every tube shows the same digit, cycling 0->9.
static void show_slot_machine(uint8_t step)
{
    uint8_t d = step % 10;
    uint8_t all[NIXIE_NUM_TUBES] = { d, d, d, d, d, d };
    nixie_set_digits(all);
}

// All tubes off (for the ringing/confirm flash "off" phase).
static void show_blank(void)
{
    uint8_t all[NIXIE_NUM_TUBES] = {
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK,
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK
    };
    nixie_set_digits(all);
}

// Show one IP octet (0..255) right-aligned, leading tubes blank. e.g. 192 ->
// "xxx192", 3 -> "xxxxx3". Colon off; this is a status readout, not a time.
static void show_octet(uint8_t v)
{
    uint8_t d[NIXIE_NUM_TUBES] = {
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK,
        NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK
    };
    d[5] = v % 10;
    if (v >= 10)  d[4] = (v / 10) % 10;
    if (v >= 100) d[3] = (v / 100) % 10;
    nixie_set_digits(d);
    nixie_set_colon(false);
}

// Capture the current STA IPv4 octets for the SHOW_IP readout. Returns false if
// we have no IP yet (not connected).
static bool capture_ip(uint8_t out[4])
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK) return false;
    if (ip.ip.addr == 0) return false;
    out[0] = esp_ip4_addr1_16(&ip.ip);
    out[1] = esp_ip4_addr2_16(&ip.ip);
    out[2] = esp_ip4_addr3_16(&ip.ip);
    out[3] = esp_ip4_addr4_16(&ip.ip);
    return true;
}

// If the RTC has a believable time, copy it into the ESP system clock so the
// display is correct from the first second, before SNTP has had a chance.
static void seed_system_clock_from_rtc(void)
{
    rtc_time_t r;
    if (rtc_get(&r) != ESP_OK || !rtc_time_valid(&r)) return;

    struct tm tm = {
        .tm_year = r.year - 1900,
        .tm_mon  = r.month - 1,
        .tm_mday = r.day,
        .tm_hour = r.hour,
        .tm_min  = r.minute,
        .tm_sec  = r.second,
        .tm_isdst = -1,
    };
    time_t epoch = mktime(&tm);            // interprets tm as local (TZ already set)
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system clock seeded from RTC");
}

// Slot 0 shows at seconds 52-53, slot 1 at 54-55, else -1 (show the time).
static int temp_slot_for_second(int sec)
{
    if (sec == 52 || sec == 53) return 0;
    if (sec == 54 || sec == 55) return 1;
    return -1;
}

// Adjust the persisted manual brightness by `delta`, clamped to 0..255.
static void nudge_brightness(int delta)
{
    clock_settings_t c;
    settings_get(&c);
    int v = (int)c.brightness + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    c.brightness = (uint8_t)v;
    settings_set(&c);   // bumps version -> main loop re-applies + MQTT echoes
    ESP_LOGI(TAG, "brightness -> %d", v);
}

// Button events (called from buttons_tick() in the main loop context, so it's
// safe to touch settings/NVS here).
static void on_button(button_event_t ev)
{
    switch (ev) {
    case BTN_BRIGHT_UP:    nudge_brightness(+BRIGHT_STEP); break;
    case BTN_BRIGHT_DOWN:  nudge_brightness(-BRIGHT_STEP); break;
    case BTN_SHOW_TEMP:
        s_force_temp_until = (esp_timer_get_time() / 1000) + SHOW_TEMP_MS;
        ESP_LOGI(TAG, "show temp now");
        break;
    case BTN_ANTIPOISON:   antipoison_trigger(); break;
    case BTN_SHOW_IP:
        if (capture_ip(s_ip_octet)) {
            // 4 octets, each gets IP_OCTET_MS (+ a short gap between).
            s_show_ip_until = (esp_timer_get_time() / 1000) + 4 * IP_OCTET_MS;
            ESP_LOGI(TAG, "show IP %u.%u.%u.%u", s_ip_octet[0], s_ip_octet[1],
                     s_ip_octet[2], s_ip_octet[3]);
        } else {
            ESP_LOGW(TAG, "show IP: no address yet");
        }
        break;
    case BTN_ALARM_TOGGLE: {
        clock_settings_t c;
        settings_get(&c);
        c.alarm_enabled = !c.alarm_enabled;
        settings_set(&c);   // bumps version -> MQTT echoes; colon cue updates
        ESP_LOGI(TAG, "alarm %s (button combo)", c.alarm_enabled ? "ON" : "OFF");
        s_confirm_until = (esp_timer_get_time() / 1000) + CONFIRM_MS;
        break;
    }
    case BTN_PROVISION:
        // Set the one-shot flag and reboot into the SoftAP portal.
        provision_request_reboot();
        break;
    case BTN_FACTORY_RESET:
        ESP_LOGW(TAG, "FACTORY RESET: erasing NVS and rebooting");
        nvs_flash_erase();
        esp_restart();
        break;
    case BTN_SNOOZE_SHORT:
        alarm_snooze();
        break;
    case BTN_SNOOZE_LONG:
        if (alarm_dismiss())   // confirm the dismiss with a brief double-blink
            s_confirm_until = (esp_timer_get_time() / 1000) + CONFIRM_MS;
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "NixieClock starting (settings + RTC + SNTP + MQTT + temps)");

    nixie_init();

    // Settings first: it also performs the one-time nvs_flash_init that WiFi
    // depends on, and gives us the initial brightness/anti-poison config.
    settings_init();

    // Provisioning gate: if requested (one-shot flag, boot-held provision
    // button, or no creds at all), bring up the SoftAP portal instead of the
    // normal clock network stack. provision_run() blocks and reboots on save.
    if (provision_requested()) {
        uint8_t mac[6];
        char suffix[8];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(suffix, sizeof(suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);
        provision_run(suffix);
    }

    clock_settings_t cfg;
    settings_get(&cfg);
    uint32_t cfg_ver = settings_version();

    temps_init();
    temps_apply_settings(&cfg);
    temps_start();

    // TZ must be set before we interpret any RTC/SNTP time as local. netclock
    // also sets it, but do it here too so the boot-time RTC seed is correct.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    i2cbus_scan();   // log every I2C device so a new/unknown chip is easy to spot
    bool have_rtc = (rtc_open() == ESP_OK);
    if (have_rtc)
        seed_system_clock_from_rtc();
    else
        ESP_LOGW(TAG, "RTC not responding; running on SNTP/system time only");

    netclock_start();
    mqttctrl_start();
    webui_start();
    antipoison_init();
    apply_antipoison(&cfg);
    buttons_init(on_button);
    buzzer_init();
    alarm_init();

    uint32_t tick = 0;
    uint8_t anim = 0;
    int64_t last_anim_ms = 0;
    int last_level = -1;     // last brightness pushed to the driver

    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        buttons_tick(now_ms);   // debounce + dispatch via on_button()

        // Pick up settings changes (web UI / MQTT / buttons) cheaply.
        if (settings_version() != cfg_ver) {
            settings_get(&cfg);
            cfg_ver = settings_version();
            apply_antipoison(&cfg);
            temps_apply_settings(&cfg);
            last_level = -1;             // force a re-apply of brightness
        }

        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        bool valid = (lt.tm_year + 1900) >= MIN_VALID_YEAR;
        int mins = lt.tm_hour * 60 + lt.tm_min;

        // Run the alarm state machine; it may start/stop the buzzer.
        if (valid) alarm_tick(&lt, &cfg, now_ms);
        bool ringing = valid && alarm_is_ringing();

        // Anti-poison only makes sense once we're showing a steady clock (the
        // slot-machine already exercises every cathode). A ringing alarm takes
        // priority over a scrub. When anti-poison runs it owns the display and
        // its own brightness, so skip the normal draw.
        if (valid && !ringing && antipoison_tick(now_ms)) {
            // routine drew this frame; it restores brightness when it ends
            last_level = -1;
        } else {
            // Apply effective brightness (manual / night / future sensor).
            int level = valid
                ? settings_effective_brightness(&cfg, mins, NO_SENSOR)
                : cfg.brightness;
            if (level != last_level) {
                nixie_set_brightness((uint8_t)level);
                last_level = level;
            }

            if (ringing) {
                // Flash the current time on/off (~1.6 Hz) while the alarm sounds.
                if ((now_ms / 300) % 2 == 0) show_time(&lt, cfg.h24);
                else                          show_blank();
                nixie_set_colon((now_ms / 300) % 2 == 0);
                vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
                continue;
            }

            // IP readout (provision button short tap): walk the 4 octets, each
            // for IP_OCTET_MS with a short blank gap so equal/adjacent octets
            // are visually distinct.
            if (now_ms < s_show_ip_until) {
                int64_t elapsed = (4 * IP_OCTET_MS) - (s_show_ip_until - now_ms);
                int idx = (int)(elapsed / IP_OCTET_MS);
                if (idx < 0) idx = 0;
                if (idx > 3) idx = 3;
                bool gap = (elapsed % IP_OCTET_MS) >= (IP_OCTET_MS - IP_GAP_MS);
                if (gap) show_blank(); else show_octet(s_ip_octet[idx]);
                nixie_set_colon(false);
                vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
                continue;
            }

            // Temperature: either the scheduled interlude near the end of each
            // minute, or a button-forced window (SHOW_TEMP). In the forced
            // window we show slot 0 for the first half, slot 1 for the second.
            bool drew_temp = false;
            if (valid && cfg.temp_enabled) {
                int slot;
                if (now_ms < s_force_temp_until) {
                    int64_t left = s_force_temp_until - now_ms;
                    slot = (left > SHOW_TEMP_MS / 2) ? 0 : 1;
                } else {
                    slot = temp_slot_for_second(lt.tm_sec);
                }
                int16_t centi;
                if (slot >= 0 && temps_get(slot, &centi)) {
                    show_temp(centi, cfg.temp_decimals, now_ms);  // drives colon itself
                    drew_temp = true;
                }
            }

            if (!drew_temp) {
                if (valid) {
                    // Dismiss-confirm: blink the whole display ~2x.
                    bool confirm = now_ms < s_confirm_until;
                    bool on = !confirm || ((now_ms / 250) % 2 == 0);
                    if (on) show_time(&lt, cfg.h24);
                    else    show_blank();

                    // Colon: shortened pulse = "alarm armed"; else normal.
                    bool armed = cfg.alarm_enabled && alarm_is_armed();
                    bool colon;
                    if (!on)         colon = false;
                    else if (armed)  colon = (now_ms % 1000) < 150;   // brief pip
                    else             colon = cfg.blink_colon ? ((now_ms / 500) % 2 == 0) : true;
                    nixie_set_colon(colon);
                } else {
                    if (now_ms - last_anim_ms >= 100) {     // ~10 steps/sec
                        last_anim_ms = now_ms;
                        show_slot_machine(anim++);
                    }
                    nixie_set_colon(false);
                }
            }
        }

        if (++tick % 200 == 0)
            ESP_LOGI(TAG, "valid=%d synced=%d rtc=%d mqtt=%d night=%d lvl=%d",
                     valid, netclock_synced(), have_rtc, mqttctrl_connected(),
                     settings_is_night(&cfg, mins), last_level);

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
