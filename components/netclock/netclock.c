#include "netclock.h"
#include "rtc_clock.h"
#include "netcfg.h"
#include "settings.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"

#define NTP_SERVER "pool.ntp.org"

static const char *TAG = "netclock";
static volatile bool s_synced = false;
static volatile uint32_t s_last_sync_s = 0;   // uptime s of the last sync (32-bit: atomic)
static bool s_sntp_started = false;

// SNTP delivered a fresh time -> mirror it into the RTC.
static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    s_last_sync_s = up ? up : 1;   // 0 means "never"

    time_t now = time(NULL);
    if (rtc_set_epoch((int64_t)now) == ESP_OK)
        ESP_LOGI(TAG, "SNTP sync -> RTC updated");
    else
        ESP_LOGW(TAG, "SNTP sync but RTC write failed");

    settings_persist_epoch((int64_t)now);   // keep the NVS fallback fresh too
}

static void start_sntp_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.sync_cb = on_time_sync;
    cfg.start = true;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    ESP_LOGI(TAG, "SNTP started against %s", NTP_SERVER);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "got IP, starting time sync");
        start_sntp_once();
    }
}

void netclock_start(void)
{
    // TZ is owned by main (from settings) and already applied before we get
    // here, so we must NOT re-force it — that would clobber a custom timezone.

    // NVS is initialised once by settings_init() before we get here.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    netcfg_t nc;
    netcfg_get(&nc);   // NVS creds if provisioned, else secrets.h fallback

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, nc.wifi_ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, nc.wifi_pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi station started, SSID=%s", nc.wifi_ssid);
}

bool netclock_synced(void)
{
    return s_synced;
}

int64_t netclock_last_sync_ms(void)
{
    return (int64_t)s_last_sync_s * 1000;
}
