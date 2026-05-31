#include "webui.h"
#include "control.h"
#include "antipoison.h"
#include "alarm.h"
#include "temps.h"
#include "netcfg.h"
#include "mqttctrl.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "webui";

// The control page. Written with single-quoted HTML/JS attributes so the C
// string only needs double quotes at its own boundaries. Values are sent as the
// POST body (no URL-encoding needed); keys are simple identifiers in the query.
static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Nixie Clock</title><style>"
"body{font-family:system-ui,sans-serif;max-width:560px;margin:1rem auto;padding:0 1rem;background:#111;color:#eee}"
"h1{font-size:1.3rem}h2{font-size:1rem;margin:1.2rem 0 .3rem;color:#fa3}"
"fieldset{border:1px solid #333;border-radius:8px;margin:0 0 .6rem}"
"label{display:flex;justify-content:space-between;align-items:center;gap:.6rem;margin:.35rem 0}"
"input,select,button{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:.3rem}"
"input[type=range]{flex:1}button{cursor:pointer;padding:.4rem .8rem}"
".s{color:#9c9;font-size:.85rem}.r{color:#f66}"
"</style></head><body><h1>Nixie Clock</h1>"
"<div class='s' id='status'>loading...</div>"

"<h2>Display</h2><fieldset>"
"<label>Brightness <input type='range' min='0' max='255' id='brightness' onchange='set(this)'></label>"
"<label>Night dimming <input type='checkbox' id='night_enabled' onchange='setck(this)'></label>"
"<label>Night brightness <input type='range' min='0' max='255' id='night_brightness' onchange='set(this)'></label>"
"<label>Night start <input type='time' id='night_start' onchange='set(this)'></label>"
"<label>Night end <input type='time' id='night_end' onchange='set(this)'></label>"
"<label>Blink colon <input type='checkbox' id='blink_colon' onchange='setck(this)'></label>"
"<label>24-hour <input type='checkbox' id='h24' onchange='setck(this)'></label>"
"</fieldset>"

"<h2>Anti-poisoning</h2><fieldset>"
"<label>Enabled <input type='checkbox' id='ap_enabled' onchange='setck(this)'></label>"
"<label>Run a scrub now <button onclick=\"act('antipoison_now')\">Scrub</button></label>"
"</fieldset>"

"<h2>Alarm</h2><fieldset>"
"<label>Enabled <input type='checkbox' id='alarm_enabled' onchange='setck(this)'></label>"
"<label>Time <input type='time' id='alarm_time' onchange='set(this)'></label>"
"<label>Melody (0-7) <input type='number' min='0' max='7' id='alarm_melody' onchange='set(this)'></label>"
"<label>Snooze (min) <input type='number' min='1' max='60' id='alarm_snooze' onchange='set(this)'></label>"
"<label>Days <span id='dow'>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Su</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Mo</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Tu</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>We</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Th</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Fr</label>"
"<label style='display:inline'><input type='checkbox' name='dow' onchange='getdow()'>Sa</label>"
"</span></label>"
"<label><span class='m'>No days ticked = every day.</span></label>"
"<label><span id='ring' class='r'></span>"
"<span><button onclick=\"act('alarm_snooze')\">Snooze</button> "
"<button onclick=\"act('alarm_dismiss')\">Dismiss</button></span></label>"
"</fieldset>"

"<h2>Temperature</h2><fieldset>"
"<label>Show on tubes <input type='checkbox' id='temp_enabled' onchange='setck(this)'></label>"
"<label>Decimals <select id='temp_decimals' onchange='set(this)'>"
"<option>0</option><option>1</option><option>2</option></select></label>"
"<label>Slot 1 source <select id='temp1_source' onchange='set(this)'>"
"<option>none</option><option>mqtt</option><option>ds18b20</option></select></label>"
"<label>Slot 1 value <span><input id='temp1_in' size='6'>"
"<button onclick=\"inp(1)\">Set</button> <span id='temp1'></span></span></label>"
"<label>Slot 1 ROM <input id='temp1_rom' size='16' onchange='set(this)'></label>"
"<label>Slot 2 source <select id='temp2_source' onchange='set(this)'>"
"<option>none</option><option>mqtt</option><option>ds18b20</option></select></label>"
"<label>Slot 2 value <span><input id='temp2_in' size='6'>"
"<button onclick=\"inp(2)\">Set</button> <span id='temp2'></span></span></label>"
"<label>Slot 2 ROM <input id='temp2_rom' size='16' onchange='set(this)'></label>"
"<label>Found sensors <span class='s' id='ds_roms'></span></label>"
"</fieldset>"

"<h2>Network</h2><fieldset>"
"<label>WiFi SSID <input id='n_ssid'></label>"
"<label>WiFi password <input id='n_pass' type='password' placeholder='(unchanged)'></label>"
"<label>MQTT host <input id='n_mhost'></label>"
"<label>MQTT port <input id='n_mport'></label>"
"<label>MQTT user <input id='n_muser'></label>"
"<label>MQTT password <input id='n_mpass' type='password' placeholder='(unchanged)'></label>"
"<label><span class='m'>WiFi changes apply on reboot; MQTT applies immediately.</span>"
"<span><button onclick='savenet()'>Save</button> "
"<button onclick=\"act('mqtt_test')\">Test MQTT</button></span></label>"
"<label>MQTT status <span id='mqtt' class='s'></span></label>"
"</fieldset>"

"<script>"
"var S={};"
"function post(u,b){return fetch(u,{method:'POST',body:b||''}).then(r=>r.json()).then(render)}"
"function set(e){post('/api/set?k='+e.id,e.value)}"
"function setck(e){post('/api/set?k='+e.id,e.checked?'ON':'OFF')}"
"function act(a){post('/api/action?a='+a)}"
"function inp(n){post('/api/action?a=temp'+n+'_input',document.getElementById('temp'+n+'_in').value)}"
"function render(s){S=s;"
"document.getElementById('status').textContent="
"s.time+'  |  '+(s.synced=='ON'?'synced':'no sync')+'  |  '+s.ip+'  |  '+s.rssi+'dBm'+(s.night_active=='ON'?'  |  night':'');"
"for(var k in s){var e=document.getElementById(k);if(!e||e===document.activeElement)continue;"
"if(e.type=='checkbox')e.checked=(s[k]=='ON');"
"else if(e.tagName=='SELECT'||e.tagName=='INPUT')e.value=s[k];"
"else e.textContent=s[k];}"
"document.getElementById('ring').textContent=(s.alarm_ringing=='ON')?'RINGING':(s.alarm_armed=='ON'?'armed':'');"
"function tv(x){return(x&&x!='unknown')?(x+'\\u00b0C'):'--';}"
"document.getElementById('temp1').textContent=tv(s.temp1);"
"document.getElementById('temp2').textContent=tv(s.temp2);"
"document.getElementById('ds_roms').textContent=s.ds_roms||'none';"
"document.getElementById('mqtt').textContent=(s.mqtt=='ON'?'connected':'not connected');"
"if(document.activeElement.name!='dow')setdow(s.alarm_dow);}"
"function setdow(m){var b=document.getElementsByName('dow');for(var i=0;i<7;i++)b[i].checked=!!(m&(1<<i));}"
"function getdow(){var b=document.getElementsByName('dow'),m=0;for(var i=0;i<7;i++)if(b[i].checked)m|=(1<<i);"
"post('/api/set?k=alarm_dow',''+m);}"
"function refresh(){fetch('/api/state').then(r=>r.json()).then(render)}"
"function loadnet(){fetch('/api/net').then(r=>r.json()).then(function(n){"
"document.getElementById('n_ssid').value=n.wifi_ssid;document.getElementById('n_mhost').value=n.mqtt_host;"
"document.getElementById('n_mport').value=n.mqtt_port;document.getElementById('n_muser').value=n.mqtt_user;});}"
"function gv(id){return encodeURIComponent(document.getElementById(id).value)}"
"function savenet(){var b='ssid='+gv('n_ssid')+'&pass='+gv('n_pass')+'&mhost='+gv('n_mhost')"
"+'&mport='+gv('n_mport')+'&muser='+gv('n_muser')+'&mpass='+gv('n_mpass');"
"fetch('/api/netset',{method:'POST',body:b}).then(r=>r.text()).then(function(t){alert(t);loadnet();});}"
"refresh();loadnet();setInterval(refresh,3000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static void send_state(httpd_req_t *req)
{
    char buf[1200];
    int n = control_state_json(buf, sizeof(buf));
    // Splice in the live MQTT-connected flag (control can't depend on mqttctrl
    // without a cycle, so add it here): replace the trailing '}' with the field.
    if (n > 0 && buf[n - 1] == '}' && n < (int)sizeof(buf) - 24)
        n += snprintf(buf + n - 1, sizeof(buf) - n + 1,
                      ",\"mqtt\":\"%s\"}", mqttctrl_connected() ? "ON" : "OFF") - 1;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
}

static esp_err_t state_get(httpd_req_t *req)
{
    send_state(req);
    return ESP_OK;
}

// Read the request body (the value) into buf; returns its length (0 if none).
static int read_body(httpd_req_t *req, char *buf, int max)
{
    int len = req->content_len;
    if (len <= 0) { buf[0] = '\0'; return 0; }
    if (len > max - 1) len = max - 1;
    int got = httpd_req_recv(req, buf, len);
    if (got <= 0) got = 0;
    buf[got] = '\0';
    return got;
}

// Pull one query param by name into out; returns true if present.
static bool query_param(httpd_req_t *req, const char *name, char *out, int max)
{
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, name, out, max) == ESP_OK;
}

static esp_err_t set_post(httpd_req_t *req)
{
    char key[24], val[64];
    if (!query_param(req, "k", key, sizeof(key))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing k");
        return ESP_OK;
    }
    int vlen = read_body(req, val, sizeof(val));
    control_apply_kv(key, val, vlen);
    send_state(req);   // hand back fresh state so the page updates in one trip
    return ESP_OK;
}

static int parse_centi(const char *s)
{
    double f = atof(s);
    return (int)(f * 100.0 + (f < 0 ? -0.5 : 0.5));
}

static esp_err_t action_post(httpd_req_t *req)
{
    char a[24], body[32];
    if (!query_param(req, "a", a, sizeof(a))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing a");
        return ESP_OK;
    }
    int blen = read_body(req, body, sizeof(body));

    if (!strcmp(a, "antipoison_now"))   antipoison_trigger();
    else if (!strcmp(a, "alarm_snooze")) alarm_snooze();
    else if (!strcmp(a, "alarm_dismiss")) alarm_dismiss();
    else if (!strcmp(a, "mqtt_test"))    mqttctrl_reconnect();
    else if (!strcmp(a, "temp1_input") && blen) temps_set_mqtt(0, parse_centi(body));
    else if (!strcmp(a, "temp2_input") && blen) temps_set_mqtt(1, parse_centi(body));
    else ESP_LOGW(TAG, "unknown action %s", a);

    send_state(req);
    return ESP_OK;
}

// Current network config (no passwords) for pre-filling the form.
static esp_err_t net_get(httpd_req_t *req)
{
    netcfg_t nc;
    netcfg_get(&nc);
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"wifi_ssid\":\"%s\",\"mqtt_host\":\"%s\",\"mqtt_port\":\"%s\",\"mqtt_user\":\"%s\"}",
        nc.wifi_ssid, nc.mqtt_host, nc.mqtt_port, nc.mqtt_user);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// urlencoded field extractor (%xx / + decoding) — same shape as provision's.
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

// Save WiFi+MQTT creds from the Network form. Empty password fields are left
// unchanged (so you don't have to retype). WiFi takes effect on next reboot;
// MQTT is applied live via mqttctrl_reconnect().
static esp_err_t netset_post(httpd_req_t *req)
{
    char body[512];
    int got = read_body(req, body, sizeof(body));
    if (got <= 0) { httpd_resp_sendstr(req, "no data"); return ESP_OK; }

    char ssid[33], pass[65], mhost[64], mport[6], muser[33], mpass[65];
    field(body, "ssid",  ssid,  sizeof(ssid));
    field(body, "pass",  pass,  sizeof(pass));
    field(body, "mhost", mhost, sizeof(mhost));
    field(body, "mport", mport, sizeof(mport));
    field(body, "muser", muser, sizeof(muser));
    field(body, "mpass", mpass, sizeof(mpass));

    bool wifi_changed = false;
    if (ssid[0]) {
        // Pass NULL for an empty password so the stored one is kept.
        netcfg_set_wifi(ssid, pass[0] ? pass : NULL);
        wifi_changed = true;
    }
    netcfg_set_mqtt(mhost[0] ? mhost : NULL, mport[0] ? mport : NULL,
                    muser, mpass[0] ? mpass : NULL);
    mqttctrl_reconnect();   // apply MQTT immediately

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, wifi_changed
        ? "Saved. MQTT applied now; WiFi change takes effect on next reboot."
        : "Saved. MQTT applied now.");
    return ESP_OK;
}

void webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/state",  .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/set",    .method = HTTP_POST, .handler = set_post },
        { .uri = "/api/action", .method = HTTP_POST, .handler = action_post },
        { .uri = "/api/net",    .method = HTTP_GET,  .handler = net_get },
        { .uri = "/api/netset", .method = HTTP_POST, .handler = netset_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(srv, &routes[i]);

    ESP_LOGI(TAG, "web UI started on :80");
}
