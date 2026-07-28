#include "control.h"
#include "settings.h"
#include "temps.h"
#include "alarm.h"
#include "netclock.h"
#include "bh1750.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
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
    case TEMP_SRC_SHELLY:  return "shelly";
    default:               return "none";
    }
}

static uint8_t src_from_name(const char *s)
{
    if (strcasecmp(s, "mqtt") == 0)    return TEMP_SRC_MQTT;
    if (strcasecmp(s, "ds18b20") == 0) return TEMP_SRC_DS18B20;
    if (strcasecmp(s, "shelly") == 0)  return TEMP_SRC_SHELLY;
    return TEMP_SRC_NONE;
}

// Shelly BLU BLE MAC <-> "AA:BB:CC:DD:EE:FF". parse returns false on malformed.
static void mac_to_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    if (!s) return false;
    unsigned b[6];
    char tail = 0;
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &tail);
    if (n != 6) return false;   // exactly six octets, nothing trailing
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

static const char *onoff(bool v) { return v ? "ON" : "OFF"; }

static const char *night_mode_name(uint8_t m)
{
    switch (m) {
    case NIGHT_MODE_SUN: return "sun";
    case NIGHT_MODE_LUX: return "lux";
    default:             return "fixed";
    }
}

static uint8_t night_mode_from_name(const char *s)
{
    if (strcasecmp(s, "sun") == 0) return NIGHT_MODE_SUN;
    if (strcasecmp(s, "lux") == 0) return NIGHT_MODE_LUX;
    return NIGHT_MODE_FIXED;
}

// Parse a coordinate in degrees ("50.08", "-14.4") into hundredths, clamped.
static int16_t parse_coord_e2(const char *s, int limit)
{
    double d = atof(s);
    int v = (int)(d * 100.0 + (d < 0 ? -0.5 : 0.5));
    if (v < -limit) v = -limit;
    if (v >  limit) v =  limit;
    return (int16_t)v;
}

// Format hundredths-of-a-degree as a decimal string ("50.08", "-14.40").
static void fmt_e2(int e2, char *buf, size_t n)
{
    int v = e2, neg = v < 0;
    if (neg) v = -v;
    snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", v / 100, v % 100);
}

// Parse a calibration offset in degrees ("1.5", "-0.5", "2", "") into centi-
// degrees. Empty/blank clears it (0). Clamped to a sane +/-50 C.
static int16_t parse_offset_centi(const char *s)
{
    if (!s || !*s) return 0;
    double d = atof(s);
    int v = (int)(d * 100.0 + (d < 0 ? -0.5 : 0.5));
    if (v < -5000) v = -5000;
    if (v >  5000) v =  5000;
    return (int16_t)v;
}

static void fmt_centi(int centi, char *buf, size_t n)
{
    int v = centi, neg = v < 0;
    if (neg) v = -v;
    snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", v / 100, v % 100);
}

// Battery percent, or "unknown" when the slot has no Shelly battery reading.
static void fmt_batt(int pct, char *buf, size_t n)
{
    if (pct < 0) snprintf(buf, n, "unknown");
    else         snprintf(buf, n, "%d", pct);
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
    else if (K("night_mode"))       c.night_mode = night_mode_from_name(val);
    else if (K("latitude"))         c.lat_e2 = parse_coord_e2(val, 9000);
    else if (K("longitude"))        c.lon_e2 = parse_coord_e2(val, 18000);
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
    else if (K("temp1_mac")) { if (vlen == 0) memset(c.shelly_mac[0], 0, 6); else if (!parse_mac(val, c.shelly_mac[0])) ok = false; }
    else if (K("temp2_mac")) { if (vlen == 0) memset(c.shelly_mac[1], 0, 6); else if (!parse_mac(val, c.shelly_mac[1])) ok = false; }
    else if (K("temp1_offset")) c.temp_offset[0] = parse_offset_centi(val);
    else if (K("temp2_offset")) c.temp_offset[1] = parse_offset_centi(val);
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

    char b0[12], b1[12];
    fmt_batt(temps_battery(0), b0, sizeof(b0));
    fmt_batt(temps_battery(1), b1, sizeof(b1));

    char rom0[TEMP_ROM_STRLEN] = "", rom1[TEMP_ROM_STRLEN] = "";
    uint64_t a;
    memcpy(&a, c.temp_rom[0], 8); if (a) temps_rom_to_str(c.temp_rom[0], rom0);
    memcpy(&a, c.temp_rom[1], 8); if (a) temps_rom_to_str(c.temp_rom[1], rom1);

    char mac0[18] = "", mac1[18] = "";
    static const uint8_t zmac[6] = {0};
    if (memcmp(c.shelly_mac[0], zmac, 6) != 0) mac_to_str(c.shelly_mac[0], mac0);
    if (memcmp(c.shelly_mac[1], zmac, 6) != 0) mac_to_str(c.shelly_mac[1], mac1);

    char off0[12], off1[12];
    fmt_centi(c.temp_offset[0], off0, sizeof(off0));
    fmt_centi(c.temp_offset[1], off1, sizeof(off1));

    char lat[12], lon[12];
    fmt_e2(c.lat_e2, lat, sizeof(lat));
    fmt_e2(c.lon_e2, lon, sizeof(lon));
    int lux = bh1750_read_lux();   // -1 when no sensor present

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

    // Running firmware version + active OTA slot — lets the web UI confirm an
    // over-the-air update took (the slot flips ota_0<->ota_1 after a flash).
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char fw[64];   // esp_app_desc version[32] + " @ " + partition label[17]
    snprintf(fw, sizeof(fw), "%s @ %s", desc ? desc->version : "?",
             run ? run->label : "?");

    return snprintf(buf, n,
        "{\"brightness\":%u,\"night_enabled\":\"%s\",\"night_mode\":\"%s\","
        "\"latitude\":\"%s\",\"longitude\":\"%s\","
        "\"bh1750\":\"%s\",\"lux\":%d,"
        "\"night_brightness\":%u,"
        "\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\","
        "\"blink_colon\":\"%s\",\"h24\":\"%s\",\"ap_enabled\":\"%s\","
        "\"alarm_enabled\":\"%s\",\"alarm_time\":\"%02u:%02u\",\"alarm_melody\":%u,"
        "\"alarm_snooze\":%u,\"alarm_dow\":%u,\"alarm_ringing\":\"%s\",\"alarm_armed\":\"%s\","
        "\"alarm_status\":\"%s\","
        "\"temp_enabled\":\"%s\",\"temp_decimals\":%u,"
        "\"temp1_source\":\"%s\",\"temp2_source\":\"%s\","
        "\"temp1_rom\":\"%s\",\"temp2_rom\":\"%s\",\"ds\":%s,"
        "\"temp1_mac\":\"%s\",\"temp2_mac\":\"%s\","
        "\"temp1_offset\":\"%s\",\"temp2_offset\":\"%s\","
        "\"temp1\":\"%s\",\"temp2\":\"%s\","
        "\"temp1_batt\":\"%s\",\"temp2_batt\":\"%s\","
        "\"night_active\":\"%s\",\"synced\":\"%s\",\"time\":\"%s\","
        "\"ip\":\"%s\",\"rssi\":%d,\"fw\":\"%s\"}",
        c.brightness, onoff(c.night_enabled), night_mode_name(c.night_mode),
        lat, lon, onoff(bh1750_present()), lux, c.night_brightness,
        c.night_start_min / 60, c.night_start_min % 60,
        c.night_end_min / 60, c.night_end_min % 60,
        onoff(c.blink_colon), onoff(c.h24), onoff(c.ap_enabled),
        onoff(c.alarm_enabled), c.alarm_hour, c.alarm_min, c.alarm_melody,
        c.alarm_snooze_min, c.alarm_dow_mask, onoff(alarm_is_ringing()),
        onoff(c.alarm_enabled && alarm_is_armed()),
        alstat,
        onoff(c.temp_enabled), c.temp_decimals,
        src_name(c.temp_src[0]), src_name(c.temp_src[1]),
        rom0, rom1, ds, mac0, mac1, off0, off1, t0, t1, b0, b1,
        onoff(settings_is_night_now(&c, now, mins)), onoff(netclock_synced()), tbuf,
        ip, rssi, fw);
}
