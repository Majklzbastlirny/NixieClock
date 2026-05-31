#include "provision.h"
#include "netcfg.h"
#include "pins.h"
#include "nixie.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "provision";

#define NS            "net"
#define FLAG_KEY      "provreq"   // one-shot "enter provisioning next boot"
#define BOOT_HOLD_MS  2000        // hold provision button this long during boot
#define KEY_SLOT      5           // provisioning button = tube slot 5 on the KEY mux

// --- entry decision ---------------------------------------------------------
static bool flag_take(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, FLAG_KEY, &v);
    if (v) { nvs_erase_key(h, FLAG_KEY); nvs_commit(h); }   // one-shot
    nvs_close(h);
    return v != 0;
}

void provision_request_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, FLAG_KEY, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "provisioning requested; rebooting");
    esp_restart();
}

// The provisioning button sits on the KEY multiplex (active-low). The nixie
// driver already pulls KEY up and samples per slot, but at this early point its
// refresh ISR may not show our slot yet, so read the raw line directly: it idles
// high and any held button pulls it low. Good enough as a boot-window gate.
static bool button_held_through_boot(void)
{
    int low = 0, total = 0;
    int64_t end = (esp_timer_get_time() / 1000) + BOOT_HOLD_MS;
    while ((esp_timer_get_time() / 1000) < end) {
        if (gpio_get_level(PIN_KEY) == 0) low++;
        total++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Require the line to be low for most of the window (a real hold), not a
    // brief multiplex blip.
    return total > 0 && low > (total * 3 / 4);
}

bool provision_requested(void)
{
    if (flag_take()) return true;
    if (!netcfg_is_provisioned()) {
        // First-ever boot with no stored creds: only force the portal if there's
        // also no compile-time fallback SSID to try.
        netcfg_t c; netcfg_get(&c);
        if (c.wifi_ssid[0] == '\0') return true;
    }
    return button_held_through_boot();
}

// --- captive DNS: answer every A query with our AP IP (192.168.4.1) ---------
static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s, (struct sockaddr *)&sa, sizeof(sa));

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;

        buf[2] |= 0x80;          // QR=1 response
        buf[3] = 0x00;           // RA=0, RCODE=0
        buf[6] = 0; buf[7] = 1;  // ANCOUNT=1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;   // NS/AR counts = 0

        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *p = buf + n;
        *p++ = 0xC0; *p++ = 0x0C;          // name -> pointer to question
        *p++ = 0x00; *p++ = 0x01;          // type A
        *p++ = 0x00; *p++ = 0x01;          // class IN
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL 60
        *p++ = 0x00; *p++ = 0x04;          // RDLENGTH 4
        *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1;          // 192.168.4.1
        sendto(s, buf, p - buf, 0, (struct sockaddr *)&from, fl);
    }
}

// --- portal pages -----------------------------------------------------------
static const char FORM_HEAD[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Nixie WiFi setup</title><style>"
"body{font-family:system-ui,sans-serif;max-width:420px;margin:1.5rem auto;padding:0 1rem}"
"label{display:block;margin:.6rem 0 .2rem}input,select{width:100%;padding:.4rem;box-sizing:border-box}"
"button{margin-top:1rem;padding:.5rem 1rem}h1{font-size:1.2rem}.m{color:#666;font-size:.85rem}"
"</style></head><body><h1>Nixie Clock WiFi</h1>"
"<form method='POST' action='/save'>"
"<label>Network</label><select name='ssid' id='ssid'></select>"
"<label>or type SSID</label><input name='ssid2' placeholder='(optional)'>"
"<label>Password</label><input name='pass' type='password'>"
"<p class='m'>Optional MQTT (leave blank to keep current):</p>"
"<label>MQTT host</label><input name='mhost'>"
"<label>MQTT port</label><input name='mport' value='1883'>"
"<label>MQTT user</label><input name='muser'>"
"<label>MQTT pass</label><input name='mpass' type='password'>"
"<button type='submit'>Save &amp; reboot</button></form>"
"<script>fetch('/scan').then(r=>r.json()).then(function(a){"
"var s=document.getElementById('ssid');a.forEach(function(n){"
"var o=document.createElement('option');o.textContent=n;o.value=n;s.appendChild(o);});});</script>"
"</body></html>";

static esp_err_t form_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, FORM_HEAD, HTTPD_RESP_USE_STRLEN);
}

// Captive-portal probe URLs (and anything unknown) -> redirect to the form.
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "", 0);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    uint16_t n = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs) esp_wifi_scan_get_ap_records(&n, recs);

    char out[1024];
    int o = 0;
    o += snprintf(out + o, sizeof(out) - o, "[");
    for (int i = 0; recs && i < n && o < (int)sizeof(out) - 40; i++) {
        o += snprintf(out + o, sizeof(out) - o, "%s\"%s\"",
                      i ? "," : "", (char *)recs[i].ssid);
    }
    o += snprintf(out + o, sizeof(out) - o, "]");
    free(recs);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, o);
}

// application/x-www-form-urlencoded field extractor with %xx / + decoding.
static int field(const char *body, const char *key, char *out, int max)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) { out[0] = '\0'; return 0; }
    p += strlen(pat);
    int o = 0;
    while (*p && *p != '&' && o < max - 1) {
        if (*p == '+') { out[o++] = ' '; p++; }
        else if (*p == '%' && p[1] && p[2]) {
            char hx[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(hx, NULL, 16);
            p += 3;
        } else out[o++] = *p++;
    }
    out[o] = '\0';
    return o;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[512];
    int len = req->content_len;
    if (len > (int)sizeof(body) - 1) len = sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[got] = '\0';

    char ssid[33], ssid2[33], pass[65];
    char mhost[64], mport[6], muser[33], mpass[65];
    field(body, "ssid",  ssid,  sizeof(ssid));
    field(body, "ssid2", ssid2, sizeof(ssid2));
    field(body, "pass",  pass,  sizeof(pass));
    field(body, "mhost", mhost, sizeof(mhost));
    field(body, "mport", mport, sizeof(mport));
    field(body, "muser", muser, sizeof(muser));
    field(body, "mpass", mpass, sizeof(mpass));

    const char *use_ssid = ssid2[0] ? ssid2 : ssid;   // typed SSID wins
    if (use_ssid[0] == '\0') {
        httpd_resp_send(req, "<p>SSID required. <a href='/'>back</a></p>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    netcfg_set_wifi(use_ssid, pass);
    if (mhost[0])
        netcfg_set_mqtt(mhost, mport[0] ? mport : "1883", muser, mpass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!doctype html><meta charset='utf-8'><body style='font-family:sans-serif'>"
        "<h1>Saved.</h1><p>The clock is rebooting and will connect to your network.</p>",
        HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "saved creds for SSID=%s; rebooting", use_ssid);
    vTaskDelay(pdMS_TO_TICKS(800));   // let the response flush
    esp_restart();
    return ESP_OK;
}

// The 8-digit AP password, shown on the tubes so only someone looking at the
// clock can join. Displayed as two 4-digit halves: first half with the colon
// lit, second half with it dark, ~2s each, looping.
static char s_pin[9];

static void pin_display_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int half = 0; half < 2; half++) {
            uint8_t d[NIXIE_NUM_TUBES] = {
                NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK,
                NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK
            };
            // 4 digits centred on tubes 1..4; colon marks which half.
            for (int i = 0; i < 4; i++)
                d[1 + i] = (uint8_t)(s_pin[half * 4 + i] - '0');
            nixie_set_digits(d);
            nixie_set_colon(half == 0);
            vTaskDelay(pdMS_TO_TICKS(2000));
            // brief blank between halves so a repeated half is distinct
            nixie_set_digits((uint8_t[]){ NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK,
                                          NIXIE_BLANK, NIXIE_BLANK, NIXIE_BLANK });
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

static void make_pin(void)
{
    for (int i = 0; i < 8; i++) s_pin[i] = '0' + (esp_random() % 10);
    s_pin[8] = '\0';
}

void provision_run(const char *node_suffix)
{
    ESP_LOGW(TAG, "entering SoftAP provisioning");
    make_pin();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "NixieClock-%s",
                     node_suffix ? node_suffix : "setup");
    ap.ap.ssid_len = n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;   // PIN shown on the tubes
    strlcpy((char *)ap.ap.password, s_pin, sizeof(ap.ap.password));

    // APSTA so we can also scan for nearby networks to list in the form.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP up: SSID=%s  PIN=%s  http://192.168.4.1/", ap.ap.ssid, s_pin);

    xTaskCreate(pin_display_task, "prov_pin", 2048, NULL, 4, NULL);

    xTaskCreate(dns_task, "prov_dns", 4096, NULL, 5, NULL);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));

    httpd_uri_t form  = { .uri = "/",      .method = HTTP_GET,  .handler = form_get };
    httpd_uri_t scan  = { .uri = "/scan",  .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t save  = { .uri = "/save",  .method = HTTP_POST, .handler = save_post };
    httpd_uri_t other = { .uri = "/*",     .method = HTTP_GET,  .handler = redirect_get };
    httpd_register_uri_handler(srv, &form);
    httpd_register_uri_handler(srv, &scan);
    httpd_register_uri_handler(srv, &save);
    httpd_register_uri_handler(srv, &other);

    // Park here; save_post() reboots on success.
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
