#include "netcfg.h"
#include "secrets.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "netcfg";
#define NS "net"

// secrets.h may or may not define the MQTT macros (older copies only had WiFi).
#ifndef MQTT_IP
#define MQTT_IP   ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT "1883"
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif

// Read one NVS string key into dst. `fallback` (secrets.h) is only used when
// `use_fallback` is true — i.e. on a never-provisioned device. Once provisioned,
// NVS is authoritative and an empty/absent key means empty (e.g. blank MQTT host
// = "no MQTT"), so a factory reset truly forgets the compiled-in defaults.
static void load_str(nvs_handle_t h, const char *key, const char *fallback,
                     bool use_fallback, char *dst, size_t dstsz)
{
    size_t len = dstsz;
    if (h && nvs_get_str(h, key, dst, &len) == ESP_OK && dst[0] != '\0')
        return;
    strlcpy(dst, use_fallback ? fallback : "", dstsz);
}

void netcfg_get(netcfg_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h = 0;
    nvs_open(NS, NVS_READONLY, &h);   // h stays 0 on failure; load_str handles it

    // secrets.h is a FIRST-BOOT convenience only. If a WiFi SSID is stored, the
    // device has been provisioned -> trust NVS exclusively, no fallback.
    bool fb = !netcfg_is_provisioned();

    load_str(h, "wifi_ssid", WIFI_SSID, fb, out->wifi_ssid, sizeof(out->wifi_ssid));
    load_str(h, "wifi_pass", WIFI_PASS, fb, out->wifi_pass, sizeof(out->wifi_pass));
    load_str(h, "mqtt_host", MQTT_IP,   fb, out->mqtt_host, sizeof(out->mqtt_host));
    load_str(h, "mqtt_port", MQTT_PORT, fb, out->mqtt_port, sizeof(out->mqtt_port));
    load_str(h, "mqtt_user", MQTT_USER, fb, out->mqtt_user, sizeof(out->mqtt_user));
    load_str(h, "mqtt_pass", MQTT_PASS, fb, out->mqtt_pass, sizeof(out->mqtt_pass));

    if (h) nvs_close(h);
}

static bool put(nvs_handle_t h, const char *key, const char *val)
{
    if (!val) return true;   // leave unchanged
    return nvs_set_str(h, key, val) == ESP_OK;
}

bool netcfg_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = put(h, "wifi_ssid", ssid) && put(h, "wifi_pass", pass);
    if (ok) ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    ESP_LOGI(TAG, "store wifi ssid=%s -> %s", ssid ? ssid : "(unchanged)", ok ? "ok" : "FAIL");
    return ok;
}

bool netcfg_set_mqtt(const char *host, const char *port,
                     const char *user, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = put(h, "mqtt_host", host) && put(h, "mqtt_port", port) &&
              put(h, "mqtt_user", user) && put(h, "mqtt_pass", pass);
    if (ok) ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool netcfg_is_provisioned(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[33];
    size_t len = sizeof(ssid);
    bool has = (nvs_get_str(h, "wifi_ssid", ssid, &len) == ESP_OK && ssid[0]);
    nvs_close(h);
    return has;
}

void netcfg_factory_reset(void)
{
    ESP_LOGW(TAG, "FACTORY RESET: erasing NVS and rebooting into setup");
    nvs_flash_erase();          // wipes creds (this "net" ns) + settings ("clock")

    // The wipe alone isn't enough: secrets.h is still compiled in as a fallback,
    // so the next boot would silently reconnect to that network. Re-init NVS and
    // set the one-shot provisioning flag (key "provreq", read by provision_requested)
    // so the device comes up in SoftAP setup mode instead, truly "forgotten".
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "provreq", 1);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(300));   // let any in-flight log/HTTP flush
    esp_restart();
    for (;;) { }                // unreachable; satisfies noreturn
}
