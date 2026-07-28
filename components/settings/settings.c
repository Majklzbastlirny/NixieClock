#include "settings.h"

#include <string.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "clock"
#define NVS_KEY_CFG   "cfg"

static clock_settings_t  s_cfg;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_ver;

// Sensible factory defaults. Night window 20:30 -> 06:00 per the design.
static void load_defaults(clock_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic   = SETTINGS_MAGIC;
    c->version = SETTINGS_VERSION;

    c->brightness       = 200;
    c->night_enabled    = true;
    c->night_mode       = NIGHT_MODE_SUN; // default: dim at real sunset/sunrise
    c->night_brightness = 40;
    c->night_start_min  = 20 * 60 + 30;   // 20:30 (fixed-mode window)
    c->night_end_min    = 6 * 60;         // 06:00
    c->lat_e2           = 5008;           // Prague 50.08 N
    c->lon_e2           = 1444;           // Prague 14.44 E
    c->auto_brightness  = false;
    c->bright_min       = 20;
    c->bright_max       = 255;
    c->blink_colon      = true;
    c->h24              = true;

    c->ap_enabled    = true;
    c->ap_period_s   = 600;
    c->ap_duration_ms = 12000;

    c->alarm_enabled    = false;
    c->alarm_hour       = 7;
    c->alarm_min        = 0;
    c->alarm_dow_mask   = 0;     // every day
    c->alarm_melody     = 0;
    c->alarm_snooze_min = 9;

    c->temp_enabled  = true;
    c->temp_decimals = 2;
    c->temp_src[0]   = TEMP_SRC_NONE;
    c->temp_src[1]   = TEMP_SRC_NONE;
    // temp_rom[][] left zeroed by the memset above.
}

// Initialise NVS for the whole app (idempotent; safe even if WiFi inits later).
static void ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

// Write the current s_cfg to NVS. Caller holds the lock.
static esp_err_t persist_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_CFG, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_init(void)
{
    ensure_nvs();
    s_lock = xSemaphoreCreateMutex();

    clock_settings_t tmp;
    size_t len = sizeof(tmp);
    nvs_handle_t h;
    bool loaded = false;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, NVS_KEY_CFG, &tmp, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(tmp) &&
            tmp.magic == SETTINGS_MAGIC && tmp.version == SETTINGS_VERSION) {
            s_cfg = tmp;
            loaded = true;
        }
    }

    if (loaded) {
        ESP_LOGI(TAG, "settings loaded from NVS (v%u)", tmp.version);
    } else {
        load_defaults(&s_cfg);
        if (persist_locked() == ESP_OK)
            ESP_LOGI(TAG, "no valid stored settings; wrote defaults");
        else
            ESP_LOGW(TAG, "no valid stored settings; using defaults (NVS write failed)");
    }
}

void settings_get(clock_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

int settings_set(const clock_settings_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *in;
    s_cfg.magic = SETTINGS_MAGIC;
    s_cfg.version = SETTINGS_VERSION;
    esp_err_t err = persist_locked();
    if (err == ESP_OK) s_ver++;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK)
        ESP_LOGW(TAG, "settings_set: NVS write failed (%s)", esp_err_to_name(err));
    return err;
}

uint32_t settings_version(void)
{
    return s_ver;
}

bool settings_is_night(const clock_settings_t *s, int mins)
{
    if (!s->night_enabled) return false;
    int a = s->night_start_min, b = s->night_end_min;
    if (a == b) return false;
    if (a < b) return mins >= a && mins < b;   // same-day window
    return mins >= a || mins < b;              // wraps past midnight
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Is the sun below the horizon at UTC epoch `utc`? Standard NOAA solar-position
// approximation, evaluated in UTC so no timezone/DST handling is needed. Returns
// true when the solar elevation is under -0.833 deg (sunset with refraction).
bool settings_sun_is_night(const clock_settings_t *s, int64_t utc)
{
    struct tm g;
    time_t t = (time_t)utc;
    gmtime_r(&t, &g);
    double lat = s->lat_e2 / 100.0;
    double lon = s->lon_e2 / 100.0;                 // east positive
    double hour = g.tm_hour + g.tm_min / 60.0 + g.tm_sec / 3600.0;   // UTC hours
    double gamma = 2.0 * M_PI / 365.0 * (g.tm_yday + (hour - 12.0) / 24.0);

    double eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma)
                    - 0.032077 * sin(gamma) - 0.014615 * cos(2 * gamma)
                    - 0.040849 * sin(2 * gamma));                    // minutes
    double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
                  - 0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma)
                  - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma); // rad

    double tst = hour * 60.0 + eqtime + 4.0 * lon;  // true solar time, minutes
    double ha = (tst / 4.0) - 180.0;                // hour angle, degrees
    double lat_r = lat * M_PI / 180.0;
    double ha_r  = ha  * M_PI / 180.0;
    double cosz = sin(lat_r) * sin(decl) + cos(lat_r) * cos(decl) * cos(ha_r);
    if (cosz > 1.0)  cosz = 1.0;
    if (cosz < -1.0) cosz = -1.0;
    double elevation = 90.0 - acos(cosz) * 180.0 / M_PI;
    return elevation < -0.833;
}

// Map ambient lux to a level between the night (dark) and day (bright) levels,
// linear in log-lux so it tracks perceived brightness.
static uint8_t lux_to_level(const clock_settings_t *s, int lux)
{
    int lo = s->night_brightness, hi = s->brightness;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    const int LUX_DARK = 3, LUX_BRIGHT = 300;
    if (lux <= LUX_DARK)   return (uint8_t)lo;
    if (lux >= LUX_BRIGHT) return (uint8_t)hi;
    double f = (log((double)lux) - log((double)LUX_DARK)) /
               (log((double)LUX_BRIGHT) - log((double)LUX_DARK));
    return (uint8_t)(lo + (int)((hi - lo) * f + 0.5));
}

bool settings_is_night_now(const clock_settings_t *s, int64_t utc, int mins)
{
    if (!s->night_enabled) return false;
    if (s->night_mode == NIGHT_MODE_FIXED) return settings_is_night(s, mins);
    return settings_sun_is_night(s, utc);
}

uint8_t settings_brightness_for(const clock_settings_t *s, int64_t utc,
                                int mins, int lux)
{
    if (!s->night_enabled) return s->brightness;
    if (s->night_mode == NIGHT_MODE_LUX && lux >= 0)
        return lux_to_level(s, lux);
    if (s->night_mode == NIGHT_MODE_FIXED)
        return settings_is_night(s, mins) ? s->night_brightness : s->brightness;
    // SUN (and LUX with no reading yet): dim between sunset and sunrise.
    return settings_sun_is_night(s, utc) ? s->night_brightness : s->brightness;
}
