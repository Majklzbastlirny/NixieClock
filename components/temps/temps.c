#include "temps.h"
#include "pins.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "onewire_bus.h"
#include "ds18b20.h"

static const char *TAG = "temps";

// A reading older than this is considered stale and not displayed. Generous so
// a once-a-minute MQTT push from HA never lapses, but short enough that a sensor
// going silent stops showing an out-of-date temperature.
#define FRESH_MS   (15 * 60 * 1000)
#define POLL_MS    30000          // DS18B20 poll interval
#define RESCAN_MS  (5 * 60 * 1000) // re-enumerate the bus occasionally
#define MAX_DS     8              // max DS18B20 devices we track

typedef struct {
    int16_t  centi;       // last value, centi-degrees C
    int64_t  ts_ms;       // when it was written (0 = never)
    uint8_t  src;         // temp_src_t, mirrored from settings
    uint8_t  rom[8];      // chosen DS18B20 ROM (when src == DS18B20)
} slot_t;

typedef struct {
    uint64_t                 addr;     // ROM address
    ds18b20_device_handle_t  dev;
} ds_dev_t;

static slot_t            s_slot[TEMP_SLOTS];
static SemaphoreHandle_t s_lock;

// 1-Wire / DS18B20 state (touched only by the poll task except the ROM list,
// which is guarded by s_lock so MQTT can read it).
static onewire_bus_handle_t s_bus;
static ds_dev_t          s_dev[MAX_DS];
static int               s_dev_count;
static uint64_t          s_roms[MAX_DS];   // snapshot for temps_list_roms()
static int               s_rom_count;
static volatile uint32_t s_scan_gen;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void temps_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < TEMP_SLOTS; i++) {
        s_slot[i].centi = TEMP_CENTI_INVALID;
        s_slot[i].ts_ms = 0;
        s_slot[i].src   = TEMP_SRC_NONE;
    }
}

void temps_apply_settings(const clock_settings_t *c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < TEMP_SLOTS; i++) {
        uint8_t newsrc = c->temp_src[i];
        bool rom_changed = memcmp(s_slot[i].rom, c->temp_rom[i], 8) != 0;
        // Switching source or ROM invalidates whatever was there.
        if (newsrc != s_slot[i].src || rom_changed) {
            s_slot[i].ts_ms = 0;
            s_slot[i].centi = TEMP_CENTI_INVALID;
        }
        s_slot[i].src = newsrc;
        memcpy(s_slot[i].rom, c->temp_rom[i], 8);
    }
    xSemaphoreGive(s_lock);
}

bool temps_get(int slot, int16_t *centi_out)
{
    if (slot < 0 || slot >= TEMP_SLOTS) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = &s_slot[slot];
    if (s->src != TEMP_SRC_NONE && s->ts_ms != 0 &&
        (now_ms() - s->ts_ms) < FRESH_MS && s->centi != TEMP_CENTI_INVALID) {
        if (centi_out) *centi_out = s->centi;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void temps_set_mqtt(int slot, int centi)
{
    if (slot < 0 || slot >= TEMP_SLOTS) return;
    if (centi < INT16_MIN + 1) centi = INT16_MIN + 1;
    if (centi > INT16_MAX) centi = INT16_MAX;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_slot[slot].src == TEMP_SRC_MQTT) {
        s_slot[slot].centi = (int16_t)centi;
        s_slot[slot].ts_ms = now_ms();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGD(TAG, "mqtt slot %d = %d", slot, centi);
}

// --- ROM string helpers -----------------------------------------------------
void temps_rom_to_str(const uint8_t rom[8], char out[TEMP_ROM_STRLEN])
{
    uint64_t a;
    memcpy(&a, rom, 8);
    snprintf(out, TEMP_ROM_STRLEN, "%016llX", (unsigned long long)a);
}

bool temps_parse_rom(const char *s, uint8_t out[8])
{
    if (!s) return false;
    // Accept exactly 16 hex chars.
    int n = 0;
    for (const char *p = s; *p; p++, n++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return false;
    }
    if (n != 16) return false;
    uint64_t a = 0;
    sscanf(s, "%llx", (unsigned long long *)&a);
    memcpy(out, &a, 8);
    return true;
}

int temps_list_roms(char out[][TEMP_ROM_STRLEN], int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_rom_count && n < max; i++) {
        uint8_t b[8];
        memcpy(b, &s_roms[i], 8);
        temps_rom_to_str(b, out[n++]);
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t temps_scan_generation(void) { return s_scan_gen; }

// --- 1-Wire bus -------------------------------------------------------------
// Enumerate the bus, (re)building the DS18B20 device table. Updates the ROM
// snapshot + scan generation if the set changed.
static void scan_bus(void)
{
    if (!s_bus) return;

    // Free any previously-created device handles.
    for (int i = 0; i < s_dev_count; i++)
        if (s_dev[i].dev) ds18b20_del_device(s_dev[i].dev);
    s_dev_count = 0;

    onewire_device_iter_handle_t iter = NULL;
    if (onewire_new_device_iter(s_bus, &iter) != ESP_OK) return;

    onewire_device_t dev;
    while (s_dev_count < MAX_DS &&
           onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        ds18b20_config_t dcfg;
        memset(&dcfg, 0, sizeof(dcfg));
        ds18b20_device_handle_t h = NULL;
        if (ds18b20_new_device(&dev, &dcfg, &h) == ESP_OK) {
            s_dev[s_dev_count].addr = dev.address;
            s_dev[s_dev_count].dev  = h;
            s_dev_count++;
        } else {
            ESP_LOGW(TAG, "device %016llX is not a DS18B20",
                     (unsigned long long)dev.address);
        }
    }
    onewire_del_device_iter(iter);

    // Publish the ROM snapshot (guarded) and bump the generation if it changed.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = (s_dev_count != s_rom_count);
    for (int i = 0; i < s_dev_count && !changed; i++)
        if (s_roms[i] != s_dev[i].addr) changed = true;
    if (changed) {
        for (int i = 0; i < s_dev_count; i++) s_roms[i] = s_dev[i].addr;
        s_rom_count = s_dev_count;
        s_scan_gen++;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "1-Wire scan: %d DS18B20 found%s", s_dev_count,
             changed ? " (changed)" : "");
}

// Read one device by address; returns centi-degrees on success.
static bool read_addr(uint64_t addr, int16_t *centi_out)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev[i].addr != addr) continue;
        if (ds18b20_trigger_temperature_conversion(s_dev[i].dev) != ESP_OK)
            return false;
        float t;
        if (ds18b20_get_temperature(s_dev[i].dev, &t) != ESP_OK)
            return false;
        int c = (int)(t * 100.0f + (t < 0 ? -0.5f : 0.5f));
        if (c < INT16_MIN + 1) c = INT16_MIN + 1;
        if (c > INT16_MAX) c = INT16_MAX;
        *centi_out = (int16_t)c;
        return true;
    }
    return false;   // assigned ROM not present on the bus
}

static void poll_task(void *arg)
{
    (void)arg;
    scan_bus();
    int64_t last_scan = now_ms();

    for (;;) {
        if (now_ms() - last_scan >= RESCAN_MS) {
            scan_bus();
            last_scan = now_ms();
        }

        for (int slot = 0; slot < TEMP_SLOTS; slot++) {
            // Snapshot what this slot wants under the lock.
            uint8_t src, rom[8];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            src = s_slot[slot].src;
            memcpy(rom, s_slot[slot].rom, 8);
            xSemaphoreGive(s_lock);

            if (src != TEMP_SRC_DS18B20) continue;
            uint64_t addr;
            memcpy(&addr, rom, 8);
            if (addr == 0) continue;            // no ROM assigned yet

            int16_t centi;
            if (read_addr(addr, &centi)) {       // blocks ~750ms in this task
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_slot[slot].src == TEMP_SRC_DS18B20) {
                    s_slot[slot].centi = centi;
                    s_slot[slot].ts_ms = now_ms();
                }
                xSemaphoreGive(s_lock);
                ESP_LOGD(TAG, "ds18b20 slot %d (%016llX) = %d",
                         slot, (unsigned long long)addr, centi);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void temps_start(void)
{
    onewire_bus_config_t bus_cfg = { .bus_gpio_num = PIN_ONEWIRE };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };
    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "1-Wire bus init failed (%s); DS18B20 disabled",
                 esp_err_to_name(err));
        s_bus = NULL;
        return;   // MQTT temp sources still work; just no DS18B20
    }
    ESP_LOGI(TAG, "1-Wire bus up on GPIO%d", PIN_ONEWIRE);
    xTaskCreate(poll_task, "temps_poll", 4096, NULL, 4, NULL);
}
