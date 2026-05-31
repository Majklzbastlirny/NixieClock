#include "settings.h"

#include <string.h>

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
    c->night_brightness = 40;
    c->night_start_min  = 20 * 60 + 30;   // 20:30
    c->night_end_min    = 6 * 60;         // 06:00
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

uint8_t settings_effective_brightness(const clock_settings_t *s, int mins, int sensor)
{
    if (s->auto_brightness && sensor >= 0) {
        int v = sensor;
        if (v < s->bright_min) v = s->bright_min;
        if (v > s->bright_max) v = s->bright_max;
        return (uint8_t)v;
    }
    return settings_is_night(s, mins) ? s->night_brightness : s->brightness;
}
