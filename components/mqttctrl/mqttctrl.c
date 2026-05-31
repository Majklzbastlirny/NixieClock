#include "mqttctrl.h"
#include "settings.h"
#include "netclock.h"
#include "rtc_clock.h"
#include "antipoison.h"
#include "temps.h"
#include "alarm.h"
#include "control.h"
#include "netcfg.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "mqttctrl";

#define SW_VERSION   "0.6"
#define DISC_PREFIX  "homeassistant"        // HA default discovery prefix
#define STATE_PERIOD_MS 5000                // periodic full-state publish
#define MAX_ROM_LIST 8                      // discovered DS18B20 ROMs to list

// Identifiers/topics, all derived from the STA MAC at start().
static char s_node[24];     // e.g. "nixie-a1b2c3"
static char s_base[40];     // e.g. "nixieclock/nixie-a1b2c3"
static char s_state[56];    // base/state
static char s_avail[56];    // base/availability
static char s_cmd[56];      // base/cmd   (we subscribe to base/cmd/#)
static char s_dev[200];     // shared HA "dev":{...} block

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected = false;

// Broker URI + credentials, kept alive for the client (it stores pointers).
static netcfg_t s_nc;
static char     s_uri[80];

// Build an MQTT config from the current netcfg creds. Refreshes s_nc/s_uri.
static esp_mqtt_client_config_t build_cfg(void)
{
    netcfg_get(&s_nc);
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%s", s_nc.mqtt_host, s_nc.mqtt_port);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.username = s_nc.mqtt_user,
        .credentials.authentication.password = s_nc.mqtt_pass,
        .session.last_will = {
            .topic = s_avail,
            .msg = "offline",
            .msg_len = 0,
            .qos = 1,
            .retain = true,
        },
        .session.keepalive = 30,
    };
    return cfg;
}

// Last MQTT-pushed temperature per slot (centi-deg), echoed back so the HA
// number entity reflects the commanded value.
static int s_temp_in[TEMP_SLOTS];

// --- small helpers ----------------------------------------------------------
// Settings parsing/validation lives in the shared control core; mqttctrl only
// needs the bits used by state publishing + the temp-input echo below.
static const char *onoff(bool v) { return v ? "ON" : "OFF"; }

static const char *src_name(uint8_t s)
{
    switch (s) {
    case TEMP_SRC_MQTT:    return "mqtt";
    case TEMP_SRC_DS18B20: return "ds18b20";
    default:               return "none";
    }
}

// Parse a decimal temperature string ("23.45", "-5.3") to centi-degrees.
static int parse_centi(const char *s)
{
    double f = atof(s);
    return (int)(f * 100.0 + (f < 0 ? -0.5 : 0.5));
}

// Format centi-degrees as "23.45" / "-5.34" into buf.
static void fmt_centi(int centi, char *buf, size_t n)
{
    int v = centi, neg = v < 0;
    if (neg) v = -v;
    snprintf(buf, n, "%s%d.%02d", neg ? "-" : "", v / 100, v % 100);
}

// --- state publishing -------------------------------------------------------
static void publish_state(void)
{
    if (!s_connected) return;

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

    // Per-slot current displayed temperature (or "unknown") and echoed input.
    char t0[12], t1[12], in0[12], in1[12];
    int16_t cv;
    if (temps_get(0, &cv)) fmt_centi(cv, t0, sizeof(t0)); else snprintf(t0, sizeof(t0), "unknown");
    if (temps_get(1, &cv)) fmt_centi(cv, t1, sizeof(t1)); else snprintf(t1, sizeof(t1), "unknown");
    fmt_centi(s_temp_in[0], in0, sizeof(in0));
    fmt_centi(s_temp_in[1], in1, sizeof(in1));

    // Assigned DS18B20 ROM per slot (16 hex, or "" if unset).
    char rom0[TEMP_ROM_STRLEN] = "", rom1[TEMP_ROM_STRLEN] = "";
    uint64_t a;
    memcpy(&a, c.temp_rom[0], 8); if (a) temps_rom_to_str(c.temp_rom[0], rom0);
    memcpy(&a, c.temp_rom[1], 8); if (a) temps_rom_to_str(c.temp_rom[1], rom1);

    // Discovered ROMs on the bus, comma-separated (for the read-only sensor).
    char roms[MAX_ROM_LIST * (TEMP_ROM_STRLEN + 1)] = "";
    char one[TEMP_ROM_STRLEN];
    char list[MAX_ROM_LIST][TEMP_ROM_STRLEN];
    int nr = temps_list_roms(list, MAX_ROM_LIST);
    for (int i = 0; i < nr; i++) {
        if (i) strlcat(roms, ",", sizeof(roms));
        snprintf(one, sizeof(one), "%s", list[i]);
        strlcat(roms, one, sizeof(roms));
    }
    if (nr == 0) snprintf(roms, sizeof(roms), "none");

    char buf[1100];
    int n = snprintf(buf, sizeof(buf),
        "{\"brightness\":%u,\"night_enabled\":\"%s\",\"night_brightness\":%u,"
        "\"night_start\":\"%02u:%02u\",\"night_end\":\"%02u:%02u\","
        "\"blink_colon\":\"%s\",\"h24\":\"%s\",\"ap_enabled\":\"%s\","
        "\"alarm_enabled\":\"%s\",\"alarm_time\":\"%02u:%02u\",\"alarm_melody\":%u,"
        "\"alarm_snooze\":%u,\"alarm_ringing\":\"%s\","
        "\"temp_enabled\":\"%s\","
        "\"temp1_src\":\"%s\",\"temp2_src\":\"%s\","
        "\"temp1_in\":%s,\"temp2_in\":%s,"
        "\"temp1_rom\":\"%s\",\"temp2_rom\":\"%s\",\"ds_roms\":\"%s\","
        "\"temp1\":\"%s\",\"temp2\":\"%s\","
        "\"night_active\":\"%s\",\"synced\":\"%s\",\"rtc\":\"%s\","
        "\"time\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
        c.brightness, onoff(c.night_enabled), c.night_brightness,
        c.night_start_min / 60, c.night_start_min % 60,
        c.night_end_min / 60, c.night_end_min % 60,
        onoff(c.blink_colon), onoff(c.h24), onoff(c.ap_enabled),
        onoff(c.alarm_enabled), c.alarm_hour, c.alarm_min, c.alarm_melody,
        c.alarm_snooze_min, onoff(alarm_is_ringing()),
        onoff(c.temp_enabled),
        src_name(c.temp_src[0]), src_name(c.temp_src[1]),
        in0, in1, rom0, rom1, roms, t0, t1,
        onoff(settings_is_night(&c, mins)), onoff(netclock_synced()),
        onoff(rtc_present()), tbuf, ip, rssi);
    if (n > 0)
        esp_mqtt_client_publish(s_client, s_state, buf, 0, 0, 0);
}

// --- HA discovery -----------------------------------------------------------
// Entity with state: `comp` is the HA platform; `field` is the JSON key in the
// state payload; `extra` carries command_topic + platform-specific keys and
// must start with a comma (or be "").
static void disc(const char *comp, const char *suffix, const char *name,
                 const char *field, const char *extra)
{
    char topic[140];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             DISC_PREFIX, comp, s_node, suffix);

    char buf[760];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"avty_t\":\"%s\","
        "\"stat_t\":\"%s\",\"val_tpl\":\"{{value_json.%s}}\"%s,%s}",
        name, s_node, suffix, s_avail, s_state, field, extra, s_dev);

    esp_mqtt_client_publish(s_client, topic, buf, 0, 1, true);
}

// Command-only entity (e.g. a button): no state topic.
static void disc_cmd(const char *comp, const char *suffix, const char *name,
                     const char *extra)
{
    char topic[140];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             DISC_PREFIX, comp, s_node, suffix);

    char buf[600];
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"avty_t\":\"%s\"%s,%s}",
        name, s_node, suffix, s_avail, extra, s_dev);

    esp_mqtt_client_publish(s_client, topic, buf, 0, 1, true);
}

static void publish_discovery(void)
{
    char ex[260];

    // Controllable: brightness (number slider)
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/brightness\",\"min\":0,\"max\":255,\"step\":1,\"mode\":\"slider\"", s_cmd);
    disc("number", "brightness", "Brightness", "brightness", ex);

    // Night dimming enable (switch)
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/night_enabled\"", s_cmd);
    disc("switch", "night_enabled", "Night dimming", "night_enabled", ex);

    // Night brightness (number)
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/night_brightness\",\"min\":0,\"max\":255,\"step\":1,\"mode\":\"slider\"", s_cmd);
    disc("number", "night_brightness", "Night brightness", "night_brightness", ex);

    // Night start / end (text HH:MM)
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/night_start\",\"pattern\":\"^[0-2][0-9]:[0-5][0-9]$\"", s_cmd);
    disc("text", "night_start", "Night start", "night_start", ex);
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/night_end\",\"pattern\":\"^[0-2][0-9]:[0-5][0-9]$\"", s_cmd);
    disc("text", "night_end", "Night end", "night_end", ex);

    // Colon blink + 24h format (switches)
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/blink_colon\"", s_cmd);
    disc("switch", "blink_colon", "Blink colon", "blink_colon", ex);
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/h24\"", s_cmd);
    disc("switch", "h24", "24-hour format", "h24", ex);

    // Anti-poison enable (switch) + manual trigger (button)
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/ap_enabled\"", s_cmd);
    disc("switch", "ap_enabled", "Anti-poisoning", "ap_enabled", ex);
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/antipoison_now\"", s_cmd);
    disc_cmd("button", "antipoison_now", "Anti-poison now", ex);

    // Alarm: enable (switch), time (text), melody (number)
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/alarm_enabled\"", s_cmd);
    disc("switch", "alarm_enabled", "Alarm", "alarm_enabled", ex);
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/alarm_time\",\"pattern\":\"^[0-2][0-9]:[0-5][0-9]$\"", s_cmd);
    disc("text", "alarm_time", "Alarm time", "alarm_time", ex);
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/alarm_melody\",\"min\":0,\"max\":7,\"step\":1", s_cmd);
    disc("number", "alarm_melody", "Alarm melody", "alarm_melody", ex);
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/alarm_snooze\",\"min\":1,\"max\":60,\"step\":1,\"unit_of_meas\":\"min\"", s_cmd);
    disc("number", "alarm_snooze", "Snooze minutes", "alarm_snooze", ex);
    // Day-of-week mask (bit0=Sun..bit6=Sat; 0=every day). Exposed as a raw 0-127
    // number; the web UI offers friendly day checkboxes that compute the same.
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/alarm_dow\",\"min\":0,\"max\":127,\"step\":1", s_cmd);
    disc("number", "alarm_dow", "Alarm days (bitmask)", "alarm_dow", ex);
    disc("binary_sensor", "alarm_ringing", "Alarm ringing", "alarm_ringing", "");
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/alarm_snooze_now\"", s_cmd);
    disc_cmd("button", "alarm_snooze_now", "Snooze", ex);
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/alarm_dismiss\"", s_cmd);
    disc_cmd("button", "alarm_dismiss", "Dismiss alarm", ex);

    // Temperature display: master enable + decimals + per-slot source/input/reading
    snprintf(ex, sizeof(ex), ",\"cmd_t\":\"%s/temp_enabled\"", s_cmd);
    disc("switch", "temp_enabled", "Temperature display", "temp_enabled", ex);
    snprintf(ex, sizeof(ex),
        ",\"cmd_t\":\"%s/temp_decimals\",\"min\":0,\"max\":2,\"step\":1", s_cmd);
    disc("number", "temp_decimals", "Temp decimals", "temp_decimals", ex);

    for (int i = 0; i < TEMP_SLOTS; i++) {
        int n = i + 1;
        char suf[20], nm[28];

        snprintf(suf, sizeof(suf), "temp%d_src", n);
        snprintf(nm, sizeof(nm), "Temp slot %d source", n);
        snprintf(ex, sizeof(ex),
            ",\"cmd_t\":\"%s/temp%d_source\",\"options\":[\"none\",\"mqtt\",\"ds18b20\"]", s_cmd, n);
        disc("select", suf, nm, suf, ex);

        snprintf(suf, sizeof(suf), "temp%d_in", n);
        snprintf(nm, sizeof(nm), "Temp slot %d input", n);
        snprintf(ex, sizeof(ex),
            ",\"cmd_t\":\"%s/temp%d_input\",\"min\":-50,\"max\":80,\"step\":0.01,"
            "\"unit_of_meas\":\"\\u00b0C\",\"mode\":\"box\"", s_cmd, n);
        disc("number", suf, nm, suf, ex);

        // DS18B20 ROM assignment for this slot (paste a 16-hex address; only
        // used when the slot source is ds18b20). See the "DS18B20 sensors"
        // sensor for the list of addresses present on the bus.
        snprintf(suf, sizeof(suf), "temp%d_rom", n);
        snprintf(nm, sizeof(nm), "Temp slot %d DS18B20 ROM", n);
        snprintf(ex, sizeof(ex),
            ",\"cmd_t\":\"%s/temp%d_rom\",\"pattern\":\"^([0-9A-Fa-f]{16})?$\"", s_cmd, n);
        disc("text", suf, nm, suf, ex);

        snprintf(suf, sizeof(suf), "temp%d", n);
        snprintf(nm, sizeof(nm), "Temp slot %d", n);
        snprintf(ex, sizeof(ex),
            ",\"unit_of_meas\":\"\\u00b0C\",\"dev_cla\":\"temperature\"");
        disc("sensor", suf, nm, suf, ex);
    }

    // Discovered DS18B20 ROM addresses on the bus (read-only, comma-separated).
    disc("sensor", "ds_roms", "DS18B20 sensors", "ds_roms", "");

    // Read-only sensors
    disc("sensor", "time", "Time", "time", "");
    snprintf(ex, sizeof(ex), ",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\"");
    disc("sensor", "rssi", "WiFi signal", "rssi", ex);
    disc("sensor", "ip", "IP address", "ip", "");
    disc("binary_sensor", "synced", "Time synced", "synced", "");
    disc("binary_sensor", "night_active", "Night active", "night_active", "");
    disc("binary_sensor", "rtc", "RTC present", "rtc", "");

    ESP_LOGI(TAG, "published HA discovery for %s", s_node);
}

// --- command handling -------------------------------------------------------
static void handle_command(const char *topic, int tlen, const char *data, int dlen)
{
    // topic = base/cmd/<name> ; isolate <name>.
    int plen = (int)strlen(s_cmd) + 1;   // base/cmd + '/'
    if (tlen <= plen) return;
    const char *name = topic + plen;
    int nlen = tlen - plen;

    char val[24];
    int vl = dlen < (int)sizeof(val) - 1 ? dlen : (int)sizeof(val) - 1;
    memcpy(val, data, vl);
    val[vl] = '\0';

    // Commands that don't touch the settings blob.
    #define ISN(s) (nlen == (int)strlen(s) && strncmp(name, s, nlen) == 0)
    if (ISN("antipoison_now")) {
        antipoison_trigger();
        ESP_LOGI(TAG, "cmd antipoison_now");
        return;
    }
    if (ISN("alarm_snooze_now")) { alarm_snooze();  publish_state(); return; }
    if (ISN("alarm_dismiss"))    { alarm_dismiss(); publish_state(); return; }
    if (ISN("temp1_input") || ISN("temp2_input")) {
        int slot = (name[4] == '2') ? 1 : 0;
        int centi = parse_centi(val);
        s_temp_in[slot] = centi;
        temps_set_mqtt(slot, centi);
        ESP_LOGI(TAG, "cmd temp%d_input = %s", slot + 1, val);
        publish_state();
        return;
    }
    #undef ISN

    // Everything else is a settings key, handled by the shared control core.
    char key[24];
    int kl = nlen < (int)sizeof(key) - 1 ? nlen : (int)sizeof(key) - 1;
    memcpy(key, name, kl);
    key[kl] = '\0';

    if (control_apply_kv(key, val, vl))
        publish_state();             // reflect the change back immediately
    else
        ESP_LOGW(TAG, "ignored cmd %s = %s", key, val);
}

// --- MQTT events ------------------------------------------------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t e = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        esp_mqtt_client_publish(s_client, s_avail, "online", 0, 1, true);
        {
            char sub[60];
            snprintf(sub, sizeof(sub), "%s/#", s_cmd);
            esp_mqtt_client_subscribe(s_client, sub, 0);
        }
        publish_discovery();
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default:
        break;
    }
}

// Periodic state publisher (also publishes promptly when settings change).
static void state_task(void *arg)
{
    (void)arg;
    uint32_t last_ver = settings_version();
    uint32_t last_scan = temps_scan_generation();
    int64_t last_pub = 0;
    for (;;) {
        if (s_connected) {
            uint32_t v = settings_version();
            uint32_t sg = temps_scan_generation();
            int64_t now = esp_timer_get_time() / 1000;
            if (v != last_ver || sg != last_scan || now - last_pub >= STATE_PERIOD_MS) {
                last_ver = v;
                last_scan = sg;
                last_pub = now;
                publish_state();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mqttctrl_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node, sizeof(s_node), "nixie-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_base, sizeof(s_base), "nixieclock/%s", s_node);
    snprintf(s_state, sizeof(s_state), "%s/state", s_base);
    snprintf(s_avail, sizeof(s_avail), "%s/availability", s_base);
    snprintf(s_cmd, sizeof(s_cmd), "%s/cmd", s_base);
    snprintf(s_dev, sizeof(s_dev),
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Nixie Clock\","
        "\"mf\":\"Michal\",\"mdl\":\"UCB32 Nixie\",\"sw\":\"%s\"}",
        s_node, SW_VERSION);

    esp_mqtt_client_config_t cfg = build_cfg();
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client started -> %s (node %s)", s_uri, s_node);

    xTaskCreate(state_task, "mqtt_state", 4096, NULL, 4, NULL);
}

void mqttctrl_reconnect(void)
{
    if (!s_client) return;
    esp_mqtt_client_config_t cfg = build_cfg();
    // Apply the new broker/credentials and bounce the connection.
    esp_mqtt_set_config(s_client, &cfg);
    esp_mqtt_client_disconnect(s_client);
    esp_mqtt_client_reconnect(s_client);
    ESP_LOGI(TAG, "MQTT reconnecting -> %s", s_uri);
}

bool mqttctrl_connected(void)
{
    return s_connected;
}
