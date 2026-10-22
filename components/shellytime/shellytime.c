// shellytime.c — see shellytime.h.
#include "shellytime.h"
#include "netclock.h"
#include "rtc_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "shellytime";

#define COMPANY_ID   0x0BA9
#define BLOCK_ID     0x0D
#define PAYLOAD_LEN  21                    // block id + 5 x u32, after the company id

#define MAX_SENDERS  8
#define FRESH_MS     15000                 // stamps older than this aren't used for a sync
#define STALE_MS     120000                // hide senders unheard for 2 min
#define REQ_WAIT_MS  25000                 // "sync now" waits this long for a beacon
#define DAILY_MS     (24LL * 3600 * 1000)
#define FIRST_DAILY_MS 60000               // first daily attempt after boot (SNTP goes first)
#define RETRY_MS     (10LL * 60 * 1000)    // daily attempt heard nothing: retry in 10 min

typedef struct {
    uint32_t unix;                         // UTC seconds at the sender
    int32_t  std_offset;                   // seconds east of UTC
    uint32_t dst_start, dst_end;           // this year's DST window (UTC), 0 = none
    int32_t  dst_shift;                    // seconds added during DST
} beacon_t;

typedef struct {
    bool     used;
    uint8_t  mac[6];                       // little-endian, as from the controller
    int8_t   rssi;
    beacon_t b;                            // latest stamp
    int64_t  stamp_ms;                     // uptime when we FIRST heard b.unix
    int64_t  last_ms;                      // uptime of the last packet (any stamp)
    bool     clean;                        // heard its previous stamp too, so stamp_ms
                                           // is the start of the burst (not mid-way)
} sender_t;

static SemaphoreHandle_t s_mtx;
static sender_t          s_snd[MAX_SENDERS];

// Last sync attempt (guarded by s_mtx; written by the main loop).
static int64_t s_last_ms;                  // uptime of the last attempt, 0 = none yet
static bool    s_last_ok;
static char    s_last_msg[112];

// Request flag (any task) and main-loop-only scheduling state.
static volatile bool s_req;
static int64_t s_pending_since;            // 0 = no manual sync waiting
static int64_t s_next_daily = FIRST_DAILY_MS;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Parse + sanity-check. Any device can send a 0x0BA9 advert, so only plausible
// values pass: a real clock (a Shelly without NTP would send ~1970), offsets
// within what timezones use, DST dates within a year of the clock.
static bool parse(const uint8_t *m, int len, beacon_t *t)
{
    if (len < 2 + PAYLOAD_LEN) return false;
    if ((m[0] | (m[1] << 8)) != COMPANY_ID || m[2] != BLOCK_ID) return false;
    const uint8_t *p = m + 3;
    t->unix       = rd_u32(p);
    t->std_offset = (int32_t)rd_u32(p + 4);
    t->dst_start  = rd_u32(p + 8);
    t->dst_end    = rd_u32(p + 12);
    t->dst_shift  = (int32_t)rd_u32(p + 16);

    const int64_t year = 400LL * 86400;
    if (t->unix < 1700000000UL || t->unix > 4102444800UL) return false;   // 2023..2100
    if (t->std_offset < -14 * 3600 || t->std_offset > 14 * 3600) return false;
    if (t->dst_shift < -2 * 3600 || t->dst_shift > 2 * 3600) return false;
    if (t->dst_start && llabs((int64_t)t->dst_start - t->unix) > year) return false;
    if (t->dst_end && llabs((int64_t)t->dst_end - t->unix) > year) return false;
    return true;
}

// UTC offset (s) the sender's zone has at UTC time `at`. Southern-hemisphere
// zones have dst_start > dst_end within the year.
static int32_t beacon_offset(const beacon_t *b, int64_t at)
{
    bool dst = false;
    if (b->dst_shift && b->dst_start != b->dst_end) {
        if (b->dst_start < b->dst_end) dst = at >= b->dst_start && at < b->dst_end;
        else                           dst = at >= b->dst_start || at < b->dst_end;
    }
    return b->std_offset + (dst ? b->dst_shift : 0);
}

// The clock's own current UTC offset in minutes, from the configured TZ.
static int clock_offset_min(time_t t)
{
    struct tm l, g;
    localtime_r(&t, &l);
    gmtime_r(&t, &g);
    int d = l.tm_yday - g.tm_yday;
    if (d > 1) d = -1;                     // across New Year
    else if (d < -1) d = 1;
    return d * 1440 + (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min);
}

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// UTC ms the sender's clock reads right now: its stamp plus the time since we first heard it.
static int64_t implied_ms(const sender_t *s, int64_t now)
{
    return (int64_t)s->b.unix * 1000 + (now - s->stamp_ms);
}

static void mac_str(const uint8_t *le, char *out /* >=18 */)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", le[5], le[4], le[3], le[2], le[1], le[0]);
}

// Caller holds s_mtx.
static sender_t *find_or_add(const uint8_t *mac)
{
    int free_i = -1, oldest = 0;
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (s_snd[i].used && memcmp(s_snd[i].mac, mac, 6) == 0) return &s_snd[i];
        if (!s_snd[i].used && free_i < 0) free_i = i;
        if (s_snd[i].last_ms < s_snd[oldest].last_ms) oldest = i;
    }
    sender_t *s = &s_snd[free_i >= 0 ? free_i : oldest];
    memset(s, 0, sizeof(*s));
    s->used = true;
    memcpy(s->mac, mac, 6);
    return s;
}

void shellytime_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
}

void shellytime_feed(const uint8_t *mac, int8_t rssi, const uint8_t *mfr, int len)
{
    beacon_t b;
    if (!s_mtx || !parse(mfr, len, &b)) return;   // most adverts end here, no lock taken
    int64_t now = now_ms();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    sender_t *s = find_or_add(mac);
    if (s->b.unix != b.unix) {
        // New stamp. If this sender's previous stamp is ~10 s older and we heard
        // it recently, we were listening when this burst began: stamp_ms is good.
        s->clean = s->stamp_ms && b.unix > s->b.unix && b.unix - s->b.unix <= 30 &&
                   now - s->last_ms < FRESH_MS;
        s->b = b;
        s->stamp_ms = now;
    }
    s->rssi = rssi;
    s->last_ms = now;
    xSemaphoreGive(s_mtx);
}

void shellytime_request_sync(void)
{
    s_req = true;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static void set_result(int64_t now, bool ok, const char *msg)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_last_ms = now;
    s_last_ok = ok;
    strlcpy(s_last_msg, msg, sizeof(s_last_msg));
    xSemaphoreGive(s_mtx);
}

// Set the clock from the fresh senders. Uses the median of their implied times
// (one Shelly with a wrong clock can't drag it), preferring "clean" stamps.
// Returns false (clock untouched) when no usable beacon is around.
static bool try_sync(const char *why, int64_t now)
{
    int64_t t[MAX_SENDERS];
    int n = 0, nclean = 0;
    char from[18] = "";

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int pass = 0; pass < 2 && n == 0; pass++) {   // pass 0: clean only; 1: any fresh
        for (int i = 0; i < MAX_SENDERS; i++) {
            sender_t *s = &s_snd[i];
            if (!s->used || now - s->stamp_ms > FRESH_MS) continue;
            if (pass == 0 && !s->clean) continue;
            t[n++] = implied_ms(s, now);
            nclean += s->clean;
            mac_str(s->mac, from);
        }
    }
    xSemaphoreGive(s_mtx);
    if (n == 0) return false;

    qsort(t, n, sizeof(t[0]), cmp_i64);
    int64_t utc_ms = t[n / 2];
    int64_t spread = t[n - 1] - t[0];
    int64_t adjust = utc_ms - wall_ms();

    struct timeval tv = { .tv_sec = (time_t)(utc_ms / 1000), .tv_usec = (suseconds_t)(utc_ms % 1000) * 1000 };
    settimeofday(&tv, NULL);
    if (rtc_present()) rtc_set_epoch((int64_t)tv.tv_sec);   // RTC holds UTC
    settings_persist_epoch((int64_t)tv.tv_sec);

    char msg[112];
    if (n == 1)
        snprintf(msg, sizeof(msg), "%s: set from %s%s, clock moved %+lld ms",
                 why, from, nclean ? "" : " (mid-burst, <=3 s late)", (long long)adjust);
    else
        snprintf(msg, sizeof(msg), "%s: median of %d Shellies (spread %lld ms), clock moved %+lld ms",
                 why, n, (long long)spread, (long long)adjust);
    ESP_LOGI(TAG, "%s", msg);
    set_result(now, true, msg);
    return true;
}

void shellytime_tick(const clock_settings_t *c, int64_t now)
{
    if (!s_mtx) return;

    // Manual request (web/MQTT/REST): apply the first usable beacon, wait a bit for one.
    if (s_req) {
        s_req = false;
        if (!s_pending_since) {
            s_pending_since = now;
            ESP_LOGI(TAG, "sync requested");
        }
    }
    if (s_pending_since) {
        if (try_sync("manual", now)) {
            s_pending_since = 0;
        } else if (now - s_pending_since > REQ_WAIT_MS) {
            s_pending_since = 0;
            ESP_LOGW(TAG, "manual sync: no Shelly time beacon heard in %d s", REQ_WAIT_MS / 1000);
            set_result(now, false, "manual: no Shelly time beacon heard in 25 s");
        }
    }

    // Optional daily sync, yielding to NTP.
    if (!c->shelly_time_daily || now < s_next_daily) return;
    int64_t ntp = netclock_last_sync_ms();
    if (ntp && now - ntp < DAILY_MS) {
        s_next_daily = ntp + DAILY_MS;     // look again once NTP would be a day stale
        char msg[112];
        snprintf(msg, sizeof(msg), "daily: skipped, NTP synced %lld min ago",
                 (long long)((now - ntp) / 60000));
        ESP_LOGI(TAG, "%s", msg);
        set_result(now, true, msg);
        return;
    }
    if (try_sync("daily", now)) {
        s_next_daily = now + DAILY_MS;
    } else {
        s_next_daily = now + RETRY_MS;
        ESP_LOGW(TAG, "daily sync: no Shelly time beacon, retrying in 10 min");
        set_result(now, false, "daily: no Shelly time beacon, retrying in 10 min");
    }
}

size_t shellytime_status_json(char *buf, size_t n)
{
    if (n < 64 || !s_mtx) { if (n) buf[0] = '\0'; return 0; }
    clock_settings_t c;
    settings_get(&c);
    int64_t now = now_ms(), wall = wall_ms();
    int64_t ntp = netclock_last_sync_ms();
    size_t o = 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    // Freshest sender's beacon -> the zone offset the Shellies use, vs. ours.
    const sender_t *fresh = NULL;
    for (int i = 0; i < MAX_SENDERS; i++)
        if (s_snd[i].used && (!fresh || s_snd[i].stamp_ms > fresh->stamp_ms)) fresh = &s_snd[i];
    char shoff[12] = "null";
    if (fresh && now - fresh->last_ms < STALE_MS)
        snprintf(shoff, sizeof(shoff), "%d", (int)(beacon_offset(&fresh->b, wall / 1000) / 60));

    o += snprintf(buf + o, n - o,
        "{\"daily\":\"%s\",\"pending\":%s,\"ntp_ago_s\":%lld,\"next_daily_s\":%lld,"
        "\"shelly_off_min\":%s,\"clock_off_min\":%d,\"last\":",
        c.shelly_time_daily ? "ON" : "OFF", s_pending_since || s_req ? "true" : "false",
        ntp ? (long long)((now - ntp) / 1000) : -1LL,
        c.shelly_time_daily ? (long long)((s_next_daily > now ? s_next_daily - now : 0) / 1000) : -1LL,
        shoff, clock_offset_min((time_t)(wall / 1000)));
    if (s_last_ms && o < n)
        o += snprintf(buf + o, n - o, "{\"ago_s\":%lld,\"ok\":%s,\"msg\":\"%s\"}",
                      (long long)((now - s_last_ms) / 1000), s_last_ok ? "true" : "false", s_last_msg);
    else if (o < n)
        o += snprintf(buf + o, n - o, "null");

    if (o < n) o += snprintf(buf + o, n - o, ",\"senders\":[");
    bool first = true;
    for (int i = 0; i < MAX_SENDERS && o + 112 < n; i++) {
        const sender_t *s = &s_snd[i];
        if (!s->used || now - s->last_ms > STALE_MS) continue;
        char ms[18];
        mac_str(s->mac, ms);
        o += snprintf(buf + o, n - o,
            "%s{\"mac\":\"%s\",\"rssi\":%d,\"age_s\":%lld,\"delta_ms\":%lld,\"clean\":%s}",
            first ? "" : ",", ms, s->rssi, (long long)((now - s->last_ms) / 1000),
            (long long)(implied_ms(s, now) - wall), s->clean ? "true" : "false");
        first = false;
    }
    xSemaphoreGive(s_mtx);
    if (o < n) o += snprintf(buf + o, n - o, "]}");
    return o < n ? o : n - 1;
}
