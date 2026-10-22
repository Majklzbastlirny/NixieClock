// blescan.c — see blescan.h.
//
// Passive BLE scan via NimBLE (observer role). For each advertisement we look
// for Service Data with 16-bit UUID 0xFCD2 (BTHome), parse the v2 object list
// (unencrypted only), keep a small registry of heard devices for the web picker,
// and feed temps_set_shelly() for any device assigned to a temperature slot.
//
// Adapted from MAX7219_Display/src/bthome.cpp (C++ -> C: a static gap callback
// instead of a lambda; pushes into temps instead of a shared app struct).
#include "blescan.h"
#include "temps.h"
#include "shellytime.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"

static const char *TAG = "blescan";

#define MAXDEV      16
#define STALE_MS    600000     // hide devices unheard for >10 min

typedef struct {
    bool     used;
    uint8_t  mac[6];           // little-endian, as delivered by the controller
    int8_t   rssi;
    int64_t  last_ms;          // esp_timer ms (64-bit: no 49.7-day wrap)
    char     name[20];
    bool     ht, hh, hb;       // has temperature / humidity / battery
    float    t, h;
    int      batt;
    int64_t  press_ms;         // last BTHome button-press event (0 = never)
} dev_t;

static dev_t             s_dev[MAXDEV];
static SemaphoreHandle_t s_mtx;

// Cached slot -> assigned Shelly MAC (display-order string), rebuilt on settings
// change and read in the scan callback. Guarded by s_mtx.
typedef struct {
    bool used;
    char mac[18];              // "AA:BB:CC:DD:EE:FF"
} slot_map_t;
static slot_map_t s_slotmap[TEMP_SLOTS];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// Standard "AA:BB:.." string (controller gives LSB first, so reverse).
static void mac_str(const uint8_t *le, char *out /* >=18 */)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             le[5], le[4], le[3], le[2], le[1], le[0]);
}

// BTHome v2 object -> data length (bytes). -1 = unknown (stop), -2 = variable.
static int obj_len(uint8_t id)
{
    switch (id) {
        case 0x00: case 0x01: return 1;
        case 0x02: case 0x03: return 2;
        case 0x04: case 0x05: return 3;
        case 0x06: case 0x07: case 0x08: return 2;
        case 0x09: return 1;
        case 0x0A: case 0x0B: return 3;
        case 0x0C: case 0x0D: return 2;
        case 0x0E: return 3;
        case 0x0F: case 0x10: case 0x11: return 1;
        case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        case 0x1E: return 2;
        case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
        case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A:
        case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 1;
        case 0x3A: return 1;
        case 0x3D: return 2;
        case 0x3E: return 4;
        case 0x3F: case 0x40: case 0x41: return 2;
        case 0x43: case 0x44: case 0x45: return 2;
        case 0x46: return 1;
        case 0x51: case 0x52: return 2;
        case 0x53: case 0x54: return -2;   // raw / text: length-prefixed
        default:   return -1;
    }
}

// Caller holds s_mtx.
static dev_t *find_or_add(const uint8_t *mac)
{
    int free_i = -1, oldest = -1;
    int64_t oldest_ms = INT64_MAX;
    for (int i = 0; i < MAXDEV; i++) {
        if (s_dev[i].used && memcmp(s_dev[i].mac, mac, 6) == 0) return &s_dev[i];
        if (!s_dev[i].used && free_i < 0) free_i = i;
        if (s_dev[i].used && s_dev[i].last_ms < oldest_ms) { oldest_ms = s_dev[i].last_ms; oldest = i; }
    }
    int idx = (free_i >= 0) ? free_i : oldest;
    dev_t *d = &s_dev[idx];
    memset(d, 0, sizeof(*d));
    d->used = true;
    memcpy(d->mac, mac, 6);
    return d;
}

// Round centi-degrees from a float °C without pulling in the math lib.
static int to_centi(float c)
{
    return (int)(c * 100.0f + (c < 0 ? -0.5f : 0.5f));
}

// Parse a BTHome v2 payload, update the registry, and feed any slot it's the
// selected sensor for.
static void bthome_handle(const uint8_t *mac, int8_t rssi,
                          const uint8_t *p, int len,
                          const uint8_t *name, int namelen)
{
    if (len < 1) return;
    if (p[0] & 0x01) return;            // encrypted: not supported
    if ((p[0] >> 5) != 2) return;       // device-info bits 5-7 == 2 -> BTHome v2

    bool ht = false, hh = false, hb = false, btn = false;
    float t = 0, h = 0; int batt = 0;

    int i = 1;
    while (i < len) {
        uint8_t id = p[i];
        int ol = obj_len(id);
        if (ol == -2) {                 // variable length: [id][len][data...]
            if (i + 1 >= len) break;
            i += 2 + p[i + 1];
            continue;
        }
        if (ol < 0) break;              // unknown id -> stop (targets are low ids)
        if (i + 1 + ol > len) break;
        const uint8_t *d = &p[i + 1];
        switch (id) {
            case 0x02: t = (int16_t)(d[0] | (d[1] << 8)) * 0.01f; ht = true; break;
            case 0x45: t = (int16_t)(d[0] | (d[1] << 8)) * 0.1f;  ht = true; break;
            case 0x03: h = (uint16_t)(d[0] | (d[1] << 8)) * 0.01f; hh = true; break;
            case 0x2E: h = d[0]; hh = true; break;
            case 0x01: batt = d[0]; hb = true; break;
            case 0x3A: if (d[0] != 0) btn = true; break;   // button event (press/long/…)
            default: break;
        }
        i += 1 + ol;
    }

    int64_t now = now_ms();
    char ms[18];
    mac_str(mac, ms);

    bool feed[TEMP_SLOTS];
    for (int s = 0; s < TEMP_SLOTS; s++) feed[s] = false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    dev_t *dev = find_or_add(mac);
    dev->rssi = rssi;
    dev->last_ms = now;
    if (name && namelen > 0) {
        int n = namelen < (int)sizeof(dev->name) - 1 ? namelen : (int)sizeof(dev->name) - 1;
        memcpy(dev->name, name, n);
        dev->name[n] = '\0';
    }
    if (ht) { dev->ht = true; dev->t = t; }
    if (hh) { dev->hh = true; dev->h = h; }
    if (hb) { dev->hb = true; dev->batt = batt; }
    if (btn) dev->press_ms = now;   // "identify" cue for the web picker
    for (int s = 0; s < TEMP_SLOTS; s++)
        if (s_slotmap[s].used && strcasecmp(ms, s_slotmap[s].mac) == 0) feed[s] = true;
    xSemaphoreGive(s_mtx);

    if (btn) ESP_LOGI(TAG, "BLU button press from %s", ms);

    for (int s = 0; s < TEMP_SLOTS; s++) {
        if (!feed[s]) continue;
        if (ht) temps_set_shelly(s, to_centi(t));
        if (hb) temps_set_shelly_battery(s, batt);
    }
}

// Walk advertisement AD structures, extract local name + BTHome service data.
static void parse_adv(const uint8_t *mac, int8_t rssi, const uint8_t *data, uint8_t len)
{
    const uint8_t *name = NULL; int namelen = 0;
    const uint8_t *sd = NULL;   int sdlen = 0;

    int i = 0;
    while (i + 1 < (int)len) {
        uint8_t flen = data[i];
        if (flen == 0 || i + 1 + flen > (int)len) break;
        uint8_t type = data[i + 1];
        const uint8_t *fd = &data[i + 2];
        int fdlen = flen - 1;
        if (type == 0x08 || type == 0x09) { name = fd; namelen = fdlen; }
        else if (type == 0x16 && fdlen >= 2 && (fd[0] | (fd[1] << 8)) == 0xFCD2) {
            sd = fd + 2; sdlen = fdlen - 2;
        }
        // Manufacturer data: maybe a Shelly gateway's time beacon (company 0x0BA9).
        // Those carry no BTHome service data, so this is independent of `sd`, and
        // the senders stay out of the s_dev picker list.
        else if (type == 0xFF) shellytime_feed(mac, rssi, fd, fdlen);
        i += flen + 1;
    }
    if (sd) bthome_handle(mac, rssi, sd, sdlen, name, namelen);
}

size_t blescan_devices_json(char *buf, size_t n)
{
    if (n < 3) { if (n) buf[0] = '\0'; return 0; }
    size_t o = 0;
    buf[o++] = '[';

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int64_t now = now_ms();
    bool first = true;
    for (int i = 0; i < MAXDEV && o < n - 160; i++) {
        if (!s_dev[i].used) continue;
        if (now - s_dev[i].last_ms > STALE_MS) continue;
        char ms[18];
        mac_str(s_dev[i].mac, ms);
        o += snprintf(buf + o, n - o,
                      "%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"age\":%u,"
                      "\"ht\":%d,\"hh\":%d,\"hb\":%d",
                      first ? "" : ",", ms, s_dev[i].name, s_dev[i].rssi,
                      (unsigned)((now - s_dev[i].last_ms) / 1000),
                      s_dev[i].ht, s_dev[i].hh, s_dev[i].hb);
        if (s_dev[i].ht) {
            int c = to_centi(s_dev[i].t) / 10;   // one decimal is plenty here
            const char *sign = c < 0 ? "-" : "";
            int a = c < 0 ? -c : c;
            o += snprintf(buf + o, n - o, ",\"t\":%s%d.%d", sign, a / 10, a % 10);
        }
        if (s_dev[i].hh) o += snprintf(buf + o, n - o, ",\"h\":%d", (int)(s_dev[i].h + 0.5f));
        if (s_dev[i].hb) o += snprintf(buf + o, n - o, ",\"b\":%d", s_dev[i].batt);
        if (s_dev[i].press_ms) {
            int64_t pa = (now - s_dev[i].press_ms) / 1000;    // seconds since press
            if (pa < 30) o += snprintf(buf + o, n - o, ",\"pa\":%u", (unsigned)pa);
        }
        o += snprintf(buf + o, n - o, "}");
        first = false;
    }
    xSemaphoreGive(s_mtx);

    o += snprintf(buf + o, n - o, "]");
    return o;
}

void blescan_apply_settings(const clock_settings_t *c)
{
    if (!s_mtx) return;   // called before blescan_start(): nothing to cache yet
    static const uint8_t zero[6] = {0};
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int s = 0; s < TEMP_SLOTS; s++) {
        if (c->temp_src[s] == TEMP_SRC_SHELLY && memcmp(c->shelly_mac[s], zero, 6) != 0) {
            s_slotmap[s].used = true;
            snprintf(s_slotmap[s].mac, sizeof(s_slotmap[s].mac),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     c->shelly_mac[s][0], c->shelly_mac[s][1], c->shelly_mac[s][2],
                     c->shelly_mac[s][3], c->shelly_mac[s][4], c->shelly_mac[s][5]);
        } else {
            s_slotmap[s].used = false;
            s_slotmap[s].mac[0] = '\0';
        }
    }
    xSemaphoreGive(s_mtx);
}

// ---- NimBLE plumbing -------------------------------------------------------
static void start_scan(void);

static int gap_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    if (e->type == BLE_GAP_EVENT_DISC)
        parse_adv(e->disc.addr.val, e->disc.rssi, e->disc.data, e->disc.length_data);
    else if (e->type == BLE_GAP_EVENT_DISC_COMPLETE)
        start_scan();
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params dp = {0};
    dp.passive = 1;
    dp.filter_duplicates = 0;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    start_scan();
    ESP_LOGI(TAG, "scanning for BTHome v2 advertisements");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void blescan_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE observer started");
}
