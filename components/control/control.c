#include "control.h"
#include "settings.h"
#include "temps.h"
#include "alarm.h"
#include "netclock.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "control";

// --- small parse helpers ----------------------------------------------------
static int hhmm_to_min(const char *s)
{
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static bool payload_on(const char *p, int len)
{
    return len >= 2 && strncasecmp(p, "ON", 2) == 0;
}

static const char *src_name(uint8_t s)
{
    switch (s) {
    case TEMP_SRC_MQTT:    return "mqtt";
    case TEMP_SRC_DS18B20: return "ds18b20";
    default:               return "none";
    }
}

static uint8_t src_from_name(const char *s)
{
    if (strcasecmp(s, "mqtt") == 0)    return TEMP_SRC_MQTT;
    if (strcasecmp(s, "ds18b20") == 0) return TEMP_SRC_DS18B20;
    return TEMP_SRC_NONE;
}

static const char *onoff(bool v) { return v ? "ON" : "OFF"; }

static void fmt_centi(int centi, char *buf, size_t n)
{
    int v = centi, neg = v < 0;
    if (neg) v = -v;
    snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", v / 100, v % 100);
}

// --- apply ------------------------------------------------------------------
bool control_apply_kv(const char *key, const char *val, int vlen)
{
    clock_settings_t c;
    settings_get(&c);
    bool known = true, ok = true;

    #define K(s) (strcmp(key, s) == 0)
    if (K("brightness"))            c.brightness = (uint8_t)atoi(val);
    else if (K("night_enabled"))    c.night_enabled = payload_on(val, vlen);
    else if (K("night_brightness")) c.night_brightness = (uint8_t)atoi(val);
    else if (K("blink_colon"))      c.blink_colon = payload_on(val, vlen);
    else if (K("h24"))              c.h24 = payload_on(val, vlen);
    else if (K("ap_enabled"))       c.ap_enabled = payload_on(val, vlen);
    else if (K("alarm_enabled"))    c.alarm_enabled = payload_on(val, vlen);
    else if (K("alarm_melody"))     c.alarm_melody = (uint8_t)atoi(val);
    else if (K("alarm_snooze"))     c.alarm_snooze_min = (uint8_t)atoi(val);
    else if (K("alarm_dow"))        c.alarm_dow_mask = (uint8_t)(atoi(val) & 0x7F);
    else if (K("temp_enabled"))     c.temp_enabled = payload_on(val, vlen);
    else if (K("temp_decimals")) { int d = atoi(val); c.temp_decimals = (uint8_t)(d < 0 ? 0 : d > 2 ? 2 : d); }
    else if (K("temp1_source"))     c.temp_src[0] = src_from_name(val);
    else if (K("temp2_source"))     c.temp_src[1] = src_from_name(val);
    else if (K("temp1_rom")) { if (vlen == 0) memset(c.temp_rom[0], 0, 8); else if (!temps_parse_rom(val, c.temp_rom[0])) ok = false; }
    else if (K("temp2_rom")) { if (vlen == 0) memset(c.temp_rom[1], 0, 8); else if (!temps_parse_rom(val, c.temp_rom[1])) ok = false; }
    else if (K("night_start")) { int m = hhmm_to_min(val); if (m >= 0) c.night_start_min = m; else ok = false; }
    else if (K("night_end"))   { int m = hhmm_to_min(val); if (m >= 0) c.night_end_min = m; else ok = false; }
    else if (K("alarm_time"))  { int m = hhmm_to_min(val); if (m >= 0) { c.alarm_hour = m / 60; c.alarm_min = m % 60; } else ok = false; }
    else known = false;
    #undef K

    if (known && ok) {
        settings_set(&c);
        ESP_LOGI(TAG, "set %s = %s", key, val);
    } else if (known) {
        ESP_LOGW(TAG, "bad value for %s: %s", key, val);
    }
    return known;
}

// --- state JSON -------------------------------------------------------------
int control_state_json(char *buf, size_t n)
{
    clock_settings_t c;
    settings_get(&c);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int mins = lt.tm_hour * 60 + lt.tm_min;
    char tbuf[24];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);

    char ip[16] = "0.0.0.0";
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));

    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    // Slot readings. Empty -> "unknown" so an HA temperature sensor treats it
    // as no-data rather than failing to parse "".
    char t0[12] = "unknown", t1[12] = "unknown";
    int16_t cv;
    if (temps_get(0, &cv)) fmt_centi(cv, t0, sizeof(t0));
    if (temps_get(1, &cv)) fmt_centi(cv, t1, sizeof(t1));

    char rom0[TEMP_ROM_STRLEN] = "", rom1[TEMP_ROM_STRLEN] = "";
    uint64_t a;
    memcpy(&a, c.temp_rom[0], 8); if (a) temps_rom_to_str(c.temp_rom[0], rom0);
    memcpy(&a, c.temp_rom[1], 8); if (a) temps_rom_to_str(c.temp_rom[1], rom1);

    // Every DS18B20 on the bus, with its live reading and which slot (if any)
    // it is assigned to: [{"rom":"..","t":"23.45","slot":1},...]. The web UI
    // renders this with per-sensor assign/unassign buttons.
    char ds[8 * 52 + 4] = "[";
    temps_ds_info_t info[8];
    int nd = temps_list_ds(info, 8);
    for (int i = 0; i < nd; i++) {
        char tv[12] = "unknown";
        if (info[i].valid) fmt_centi(info[i].centi, tv, sizeof(tv));
        int slot = strcmp(info[i].rom, rom0) == 0 ? 1
                 : strcmp(info[i].rom, rom1) == 0 ? 2 : 0;
        char one[64];
        snprintf(one, sizeof(one), "%s{\"rom\":\"%s\",\"t\":\"%s\",\"slot\":%d}",
                 i ? "," : "", info[i].rom, tv, slot);
        strlcat(ds, one, sizeof(ds));
    }
    strlcat(ds, "]", sizeof(ds));

    char alstat[24];
    alarm_status_str(&c, &lt, alstat, sizeof(alstat));

    return snprintf(buf, n,
        "{\"brightness\":%u,\"night_enabled\":\"%s\",\"night_brightness\":%u,"
        "\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\","
        "\"blink_colon\":\"%s\",\"h24\":\"%s\",\"ap_enabled\":\"%s\","
        "\"alarm_enabled\":\"%s\",\"alarm_time\":\"%02u:%02u\",\"alarm_melody\":%u,"
        "\"alarm_snooze\":%u,\"alarm_dow\":%u,\"alarm_ringing\":\"%s\",\"alarm_armed\":\"%s\","
        "\"alarm_status\":\"%s\","
        "\"temp_enabled\":\"%s\",\"temp_decimals\":%u,"
        "\"temp1_source\":\"%s\",\"temp2_source\":\"%s\","
        "\"temp1_rom\":\"%s\",\"temp2_rom\":\"%s\",\"ds\":%s,"
        "\"temp1\":\"%s\",\"temp2\":\"%s\","
        "\"night_active\":\"%s\",\"synced\":\"%s\",\"time\":\"%s\","
        "\"ip\":\"%s\",\"rssi\":%d}",
        c.brightness, onoff(c.night_enabled), c.night_brightness,
        c.night_start_min / 60, c.night_start_min % 60,
        c.night_end_min / 60, c.night_end_min % 60,
        onoff(c.blink_colon), onoff(c.h24), onoff(c.ap_enabled),
        onoff(c.alarm_enabled), c.alarm_hour, c.alarm_min, c.alarm_melody,
        c.alarm_snooze_min, c.alarm_dow_mask, onoff(alarm_is_ringing()),
        onoff(c.alarm_enabled && alarm_is_armed()),
        alstat,
        onoff(c.temp_enabled), c.temp_decimals,
        src_name(c.temp_src[0]), src_name(c.temp_src[1]),
        rom0, rom1, ds, t0, t1,
        onoff(settings_is_night(&c, mins)), onoff(netclock_synced()), tbuf,
        ip, rssi);
}
