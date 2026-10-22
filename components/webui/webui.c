#include "webui.h"
#include "control.h"
#include "antipoison.h"
#include "alarm.h"
#include "temps.h"
#include "netcfg.h"
#include "mqttctrl.h"
#include "nixie.h"
#include "weblog.h"
#include "blescan.h"
#include "shellytime.h"
#include "settings.h"
#include "rtc_clock.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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
".ds{display:flex;justify-content:space-between;align-items:center;gap:.5rem;margin:.3rem 0}"
".ds code{color:#fa3}.ds .t{color:#eee;min-width:4rem;text-align:right}"
".ds button{padding:.2rem .5rem;font-size:.8rem}"
"</style></head><body><h1>Nixie Clock</h1>"
"<div class='s' id='status'>loading...</div>"

"<h2>Display</h2><fieldset>"
"<label>Brightness <input type='range' min='0' max='255' id='brightness' onchange='set(this)'></label>"
"<label>Night dimming <input type='checkbox' id='night_enabled' onchange='setck(this)'></label>"
"<label>Night mode <select id='night_mode' onchange='set(this)'>"
"<option>fixed</option><option>sun</option><option id='opt_lux'>lux</option></select></label>"
"<label>Night brightness <input type='range' min='0' max='255' id='night_brightness' onchange='set(this)'></label>"
"<div class='s' id='fixedrow'>Fixed window:</div>"
"<label>Night start <input type='time' id='night_start' onchange='set(this)'></label>"
"<label>Night end <input type='time' id='night_end' onchange='set(this)'></label>"
"<div class='s' id='sunrow'>Sunrise/sunset location (offline):</div>"
"<label>Latitude <input id='latitude' size='8' onchange='set(this)'></label>"
"<label>Longitude <input id='longitude' size='8' onchange='set(this)'></label>"
"<label>Twilight offset (min) <input type='number' min='-120' max='120' id='sun_offset' onchange='set(this)'></label>"
"<label><span class='s'>+ = dim later after sunset and brighten earlier before "
"sunrise (stay bright through twilight); &minus; = the opposite. Approximate minutes.</span></label>"
"<div class='s' id='luxrow'>Light sensor: <span id='luxval'>-</span></div>"
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
"<label>Melody (0-7) <span><input type='number' min='0' max='7' id='alarm_melody' onchange='set(this)' size='3'> "
"<button onclick=\"act('alarm_test')\">Play</button></span></label>"
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
"<label>Status <span id='ring' class='s'></span></label>"
"<label>When ringing <span><button onclick=\"act('alarm_snooze')\">Snooze</button> "
"<button onclick=\"act('alarm_dismiss')\">Dismiss</button></span></label>"
"</fieldset>"

"<h2>Temperature</h2><fieldset>"
"<label>Show on tubes <input type='checkbox' id='temp_enabled' onchange='setck(this)'></label>"
"<label>Decimals <select id='temp_decimals' onchange='set(this)'>"
"<option>0</option><option>1</option><option>2</option></select></label>"
"<label>Slot 1 source <select id='temp1_source' onchange='set(this)'>"
"<option>none</option><option>mqtt</option><option>ds18b20</option><option>shelly</option></select></label>"
"<label>Slot 1 value <span><input id='temp1_in' size='6'>"
"<button onclick=\"inp(1)\">Set</button> <span id='temp1'></span></span></label>"
"<label>Slot 1 offset &deg;C <input id='temp1_offset' size='6' onchange='set(this)'></label>"
"<label>Slot 2 source <select id='temp2_source' onchange='set(this)'>"
"<option>none</option><option>mqtt</option><option>ds18b20</option><option>shelly</option></select></label>"
"<label>Slot 2 value <span><input id='temp2_in' size='6'>"
"<button onclick=\"inp(2)\">Set</button> <span id='temp2'></span></span></label>"
"<label>Slot 2 offset &deg;C <input id='temp2_offset' size='6' onchange='set(this)'></label>"
"<div class='s' style='margin:.5rem 0 .2rem'>DS18B20 sensors on the bus "
"(assign one to a slot to show it on the tubes):</div>"
"<div id='dslist' class='s'>scanning...</div>"
"<div class='s' style='margin:.6rem 0 .2rem'>Shelly BLU sensors seen over BLE "
"(assign one to a slot; the source becomes 'shelly'):</div>"
"<div id='shellylist' class='s'>scanning...</div>"
"</fieldset>"

"<h2>Interludes</h2><fieldset>"
"<label><span class='m'>Which second of each minute each readout shows, and for "
"how long (seconds). Keep the windows from overlapping.</span></label>"
"<label>Temp 1 at second <span><input type='number' min='0' max='59' id='temp1_sec' onchange='set(this)'>"
" for <input type='number' min='1' max='8' id='temp1_len' onchange='set(this)'></span></label>"
"<label>Temp 2 at second <span><input type='number' min='0' max='59' id='temp2_sec' onchange='set(this)'>"
" for <input type='number' min='1' max='8' id='temp2_len' onchange='set(this)'></span></label>"
"<label>Show date <input type='checkbox' id='date_enabled' onchange='setck(this)'></label>"
"<label>Date at second <span><input type='number' min='0' max='59' id='date_sec' onchange='set(this)'>"
" for <input type='number' min='1' max='8' id='date_len' onchange='set(this)'></span></label>"
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

"<h2>Clock &amp; time</h2><fieldset>"
"<label>Timezone (POSIX) <span><input id='tz' size='20' onchange='set(this)'>"
"<button onclick='useoff()' title='fill from this device (no DST)'>&#8592; phone</button></span></label>"
"<label><span class='m'>Set the clock from this phone/PC — handy off-grid when "
"the RTC has reset. Sends this device's time; the TZ above is applied too.</span></label>"
"<label>This device <span id='phtime' class='s'>--:--:--</span></label>"
"<label><span id='phzone' class='m'></span>"
"<button onclick='settime()'>Set clock from this device</button></label>"
"<div class='s' style='margin:.6rem 0 .2rem'>Shelly BLE time &mdash; the time beacon nearby "
"Shelly gateways broadcast (what the BLU H&amp;T Display syncs from). NTP stays primary.</div>"
"<label>Daily sync from Shelly <input type='checkbox' id='shelly_time_daily' onchange='setck(this)'></label>"
"<label><span class='m'>Once a day, and only while NTP hasn't synced for 24 h.</span>"
"<button onclick='shsync()'>Sync from Shelly now</button></label>"
"<div id='shtime' class='s' style='white-space:pre-line'>...</div>"
"</fieldset>"

"<h2>Firmware</h2><fieldset>"
"<label>Running <span id='fw' class='s'></span></label>"
"<label>Update <span><input type='file' id='fwfile' accept='.bin'>"
"<button onclick='otaup()'>Upload</button></span></label>"
"<label><progress id='otap' value='0' max='100' style='display:none;flex:1'></progress>"
"<span id='otamsg' class='s'></span></label>"
"<label><span class='m'>Pick the built firmware.bin; the device flashes the "
"spare slot and reboots into it.</span></label>"
"</fieldset>"

"<h2>Logs</h2><fieldset>"
"<label>Device log <span><label style='display:inline'><input type='checkbox' id='logauto' onchange='logtick()'>auto</label> "
"<button onclick='loadlog()'>Refresh</button></span></label>"
"<pre id='logbox' style='max-height:15rem;overflow:auto;background:#000;color:#9c9;"
"font-size:.72rem;line-height:1.25;padding:.5rem;white-space:pre-wrap;margin:.3rem 0'></pre>"
"</fieldset>"

"<h2>System</h2><fieldset>"
"<label><span class='m'>Erases WiFi/MQTT credentials and all settings, then reboots into setup.</span>"
"<button onclick='freset()' style='color:#f66'>Factory reset</button></label>"
"</fieldset>"

"<script>"
"var S={};"
"function post(u,b){return fetch(u,{method:'POST',body:b||''}).then(r=>r.json()).then(render)}"
"function set(e){post('/api/set?k='+e.id,e.value)}"
"function setck(e){post('/api/set?k='+e.id,e.checked?'ON':'OFF')}"
"function act(a){post('/api/action?a='+a)}"
"function inp(n){post('/api/action?a=temp'+n+'_input',document.getElementById('temp'+n+'_in').value)}"
// POSIX fixed-offset (no DST) for this device, e.g. UTC+2 -> "<+0200>-2".
"function poff(){var o=-new Date().getTimezoneOffset(),s=o<0?'-':'+',a=Math.abs(o),"
"hh=Math.floor(a/60),mm=a%60,p2=function(n){return(n<10?'0':'')+n};"
"return'<'+s+p2(hh)+p2(mm)+'>'+(o<0?'':'-')+hh+(mm?(':'+p2(mm)):'');}"
"function useoff(){var e=document.getElementById('tz');e.value=poff();set(e);}"
"function settime(){var ep=Math.floor(Date.now()/1000),tz=document.getElementById('tz').value;"
"post('/api/settime?epoch='+ep+'&tz='+encodeURIComponent(tz));}"
"function phclock(){var d=new Date();"
"var e=document.getElementById('phtime');if(!e)return;"
"e.textContent=d.toLocaleTimeString();"
"var o=-d.getTimezoneOffset(),s=o<0?'-':'+',a=Math.abs(o),p2=function(n){return(n<10?'0':'')+n};"
"try{var z=Intl.DateTimeFormat().resolvedOptions().timeZone;}catch(x){z='';}"
"document.getElementById('phzone').textContent=z+' (UTC'+s+p2(Math.floor(a/60))+':'+p2(a%60)+')';}"
"setInterval(phclock,1000);phclock();"
"function render(s){S=s;"
"document.getElementById('status').textContent="
"s.time+'  |  '+(s.synced=='ON'?'synced':'no sync')+'  |  '+s.ip+'  |  '+s.rssi+'dBm'+(s.night_active=='ON'?'  |  night':'');"
"for(var k in s){var e=document.getElementById(k);if(!e||e===document.activeElement)continue;"
"if(e.type=='checkbox')e.checked=(s[k]=='ON');"
"else if(e.tagName=='SELECT'||e.tagName=='INPUT')e.value=s[k];"
"else e.textContent=s[k];}"
"var rg=document.getElementById('ring');rg.textContent=s.alarm_status||'';"
"rg.className=(s.alarm_ringing=='ON')?'r':'s';"
"function tv(x){return(x&&x!='unknown')?(x+'\\u00b0C'):'--';}"
"function bt(x){return(x&&x!='unknown')?(' \\u00b7 bat '+x+'%'+(+x<=20?' \\u26a0 low':'')):'';}"
"document.getElementById('temp1').textContent=tv(s.temp1)+bt(s.temp1_batt);"
"document.getElementById('temp2').textContent=tv(s.temp2)+bt(s.temp2_batt);"
"document.getElementById('opt_lux').style.display=(s.bh1750=='ON')?'':'none';"
"document.getElementById('luxval').textContent=(s.bh1750=='ON')?(s.lux+' lx'):'no BH1750 detected';"
"var nm=s.night_mode;"
"document.getElementById('fixedrow').style.opacity=(nm=='fixed')?1:.4;"
"document.getElementById('sunrow').style.opacity=(nm=='sun')?1:.4;"
"document.getElementById('luxrow').style.opacity=(nm=='lux')?1:.4;"
"dslist(s.ds||[]);"
"document.getElementById('mqtt').textContent=(s.mqtt=='ON'?'connected':'not connected');"
"if(document.activeElement.name!='dow')setdow(s.alarm_dow);}"
"function setdow(m){var b=document.getElementsByName('dow');for(var i=0;i<7;i++)b[i].checked=!!(m&(1<<i));}"
"function getdow(){var b=document.getElementsByName('dow'),m=0;for(var i=0;i<7;i++)if(b[i].checked)m|=(1<<i);"
"post('/api/set?k=alarm_dow',''+m);}"
"function dslist(a){var h='';for(var i=0;i<a.length;i++){var d=a[i];"
"var t=(d.t&&d.t!='unknown')?(d.t+'\\u00b0C'):'--';"
"h+=\"<div class='ds'><code>\"+d.rom+\"</code><span class='t'>\"+t+\"</span><span>\";"
"if(d.slot)h+='slot '+d.slot+' <button onclick=dsun('+d.slot+')>Unassign</button>';"
"else h+='<button onclick=dsas(1,'+i+')>&#8594;1</button> <button onclick=dsas(2,'+i+')>&#8594;2</button>';"
"h+='</span></div>';}"
"document.getElementById('dslist').innerHTML=a.length?h:'none found';}"
"function dsas(n,i){var rom=(S.ds&&S.ds[i])?S.ds[i].rom:'';if(!rom)return;"
"post('/api/set?k=temp'+n+'_source','ds18b20').then(function(){post('/api/set?k=temp'+n+'_rom',rom);});}"
"function dsun(n){post('/api/set?k=temp'+n+'_source','none').then(function(){post('/api/set?k=temp'+n+'_rom','');});}"
"var SH=[];"
"function shlist(a){SH=a;var h='';for(var i=0;i<a.length;i++){var d=a[i];"
"var v=[];if(d.ht)v.push(d.t+'\\u00b0C');if(d.hh)v.push(d.h+'%');if(d.hb)v.push('bat '+d.b+'%'+(d.b<=20?' \\u26a0':''));"
"var as=(S.temp1_mac==d.mac)?1:((S.temp2_mac==d.mac)?2:0);"
"var hl=(d.pa!=null&&d.pa<15);"
"var st=hl?\" style='background:#4a3d0a;border-radius:6px;box-shadow:0 0 0 2px #fa3'\":'';"
"h+=\"<div class='ds'\"+st+\"><span>\"+(hl?'\\ud83d\\udd26 ':'')+\"<code>\"+d.mac+\"</code> <span class='s'>\"+(d.name||'')+' '+d.rssi+'dBm'+(v.length?' '+v.join(' '):'')+(hl?' \\u2014 pressed':'')+\"</span></span><span>\";"
"if(as)h+='slot '+as+' <button onclick=shun('+as+')>Unassign</button>';"
"else h+='<button onclick=shas(1,'+i+')>&#8594;1</button> <button onclick=shas(2,'+i+')>&#8594;2</button>';"
"h+='</span></div>';}"
"document.getElementById('shellylist').innerHTML=a.length?h:'none seen yet';}"
"function shas(n,i){var mac=(SH[i])?SH[i].mac:'';if(!mac)return;"
"post('/api/set?k=temp'+n+'_source','shelly').then(function(){post('/api/set?k=temp'+n+'_mac',mac);});}"
"function shun(n){post('/api/set?k=temp'+n+'_source','none').then(function(){post('/api/set?k=temp'+n+'_mac','');});}"
"function loadshelly(){fetch('/api/blescan').then(r=>r.json()).then(shlist);}"
// Shelly BLE time panel: last result, NTP/daily state, zone check, per-sender delta.
"function dur(s){return s<90?s+' s':s<5400?Math.round(s/60)+' min':Math.floor(s/3600)+' h '+Math.round(s%3600/60)+' min';}"
"function shtime(d){var t=[];"
"if(d.pending)t.push('waiting for a Shelly time beacon...');"
"t.push(d.last?((d.last.ok?'\\u2713 ':'\\u2717 ')+d.last.msg+' ('+dur(d.last.ago_s)+' ago)'):'no Shelly sync yet');"
"t.push('NTP: '+(d.ntp_ago_s<0?'not synced this boot':'synced '+dur(d.ntp_ago_s)+' ago')"
"+(d.daily=='ON'?' \\u00b7 daily check in '+dur(d.next_daily_s):''));"
"if(d.shelly_off_min!=null)t.push('UTC offset: Shellies '+(d.shelly_off_min>=0?'+':'')+d.shelly_off_min+' min, clock '"
"+(d.clock_off_min>=0?'+':'')+d.clock_off_min+' min'+(d.shelly_off_min!=d.clock_off_min?' \\u26a0 TZ differs':''));"
"var a=d.senders||[];if(!a.length)t.push('no Shelly time beacons heard (needs a Gen2+ Shelly, fw 2.x, acting as BLE gateway)');"
"for(var i=0;i<a.length;i++){var x=a[i];t.push(x.mac+'  '+x.rssi+' dBm  '+x.age_s+' s ago  \\u0394 '"
"+(x.delta_ms>=0?'+':'')+x.delta_ms+' ms vs clock'+(x.clean?'':' (mid-burst)'));}"
"document.getElementById('shtime').textContent=t.join('\\n');}"
"function loadshtime(){fetch('/api/shellytime').then(r=>r.json()).then(shtime);}"
"function shsync(){act('shelly_sync');document.getElementById('shtime').textContent='syncing...';"
"setTimeout(loadshtime,800);setTimeout(loadshtime,3000);}"
"function refresh(){fetch('/api/state').then(r=>r.json()).then(render)}"
"function loadnet(){fetch('/api/net').then(r=>r.json()).then(function(n){"
"document.getElementById('n_ssid').value=n.wifi_ssid;document.getElementById('n_mhost').value=n.mqtt_host;"
"document.getElementById('n_mport').value=n.mqtt_port;document.getElementById('n_muser').value=n.mqtt_user;});}"
"function gv(id){return encodeURIComponent(document.getElementById(id).value)}"
"function savenet(){var b='ssid='+gv('n_ssid')+'&pass='+gv('n_pass')+'&mhost='+gv('n_mhost')"
"+'&mport='+gv('n_mport')+'&muser='+gv('n_muser')+'&mpass='+gv('n_mpass');"
"fetch('/api/netset',{method:'POST',body:b}).then(r=>r.text()).then(function(t){alert(t);loadnet();});}"
"function freset(){if(confirm('Factory reset? This erases WiFi/MQTT credentials and all settings, then reboots into setup mode.'))"
"fetch('/api/action?a=factory_reset',{method:'POST',body:'RESET'});}"
"function otaup(){var f=document.getElementById('fwfile').files[0];if(!f){alert('pick a .bin');return;}"
"var x=new XMLHttpRequest();x.open('POST','/api/ota');"
"var p=document.getElementById('otap'),m=document.getElementById('otamsg');"
"p.style.display='inline-block';p.value=0;m.textContent='uploading...';"
"x.upload.onprogress=function(e){if(e.lengthComputable)p.value=100*e.loaded/e.total;};"
"x.onload=function(){if(x.status==200){m.textContent='OK \\u2014 rebooting...';"
"setTimeout(function(){location.reload();},9000);}else{m.textContent='failed: '+x.responseText;}};"
"x.onerror=function(){m.textContent='upload failed';};x.send(f);}"
"function loadlog(){fetch('/api/log').then(r=>r.text()).then(function(t){"
"var b=document.getElementById('logbox');var atbot=b.scrollTop+b.clientHeight>=b.scrollHeight-4;"
"b.textContent=t;if(atbot)b.scrollTop=b.scrollHeight;});}"
"var logt=null;"
"function logtick(){if(document.getElementById('logauto').checked){if(!logt)logt=setInterval(loadlog,3000);}"
"else{clearInterval(logt);logt=null;}}"
"refresh();loadnet();loadlog();loadshelly();loadshtime();setInterval(refresh,3000);setInterval(loadshelly,3000);"
"setInterval(loadshtime,10000);"   // debug panel: slower, keeps sockets/requests down
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static void send_state(httpd_req_t *req)
{
    char buf[2048];
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
    char q[192];   // roomy: /api/settime carries a URL-encoded POSIX TZ
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

// GET alias: /api/set?k=<key>&v=<value>. Same effect as the POST form but the
// value rides in the query, so it's callable from a browser bar / curl / HA
// shell_command without a body.
static esp_err_t set_get(httpd_req_t *req)
{
    char key[24], val[64];
    if (!query_param(req, "k", key, sizeof(key))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing k");
        return ESP_OK;
    }
    if (!query_param(req, "v", val, sizeof(val))) val[0] = '\0';
    control_apply_kv(key, val, (int)strlen(val));
    send_state(req);
    return ESP_OK;
}

static int parse_centi(const char *s)
{
    double f = atof(s);
    return (int)(f * 100.0 + (f < 0 ? -0.5 : 0.5));
}

// Dispatch an action with its value (POST body or GET ?v=). Returns false only
// for factory_reset without the token (so the caller can 400). Used by both the
// POST and GET action handlers.
static bool do_action(const char *a, const char *val, int vlen)
{
    if (!strcmp(a, "factory_reset")) {
        // Require the token from the confirmed UI press before wiping NVS.
        if (vlen == 5 && !strncmp(val, "RESET", 5))
            netcfg_factory_reset();   // erases NVS + reboots (does not return)
        return false;                 // no/!token -> caller reports the error
    }

    if (!strcmp(a, "antipoison_now"))   antipoison_trigger();
    else if (!strcmp(a, "alarm_snooze")) alarm_snooze();
    else if (!strcmp(a, "alarm_dismiss")) alarm_dismiss();
    else if (!strcmp(a, "alarm_test"))   alarm_preview_melody();
    else if (!strcmp(a, "mqtt_test"))    mqttctrl_reconnect();
    else if (!strcmp(a, "shelly_sync"))  shellytime_request_sync();   // main loop applies it
    else if (!strcmp(a, "temp1_input") && vlen) temps_set_mqtt(0, parse_centi(val));
    else if (!strcmp(a, "temp2_input") && vlen) temps_set_mqtt(1, parse_centi(val));
    else ESP_LOGW(TAG, "unknown action %s", a);
    return true;
}

static esp_err_t action_post(httpd_req_t *req)
{
    char a[24], body[32];
    if (!query_param(req, "a", a, sizeof(a))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing a");
        return ESP_OK;
    }
    int blen = read_body(req, body, sizeof(body));
    if (!do_action(a, body, blen)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "factory_reset needs token");
        return ESP_OK;
    }
    send_state(req);
    return ESP_OK;
}

// GET alias: /api/action?a=<action>&v=<value>.
static esp_err_t action_get(httpd_req_t *req)
{
    char a[24], val[32];
    if (!query_param(req, "a", a, sizeof(a))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing a");
        return ESP_OK;
    }
    if (!query_param(req, "v", val, sizeof(val))) val[0] = '\0';
    if (!do_action(a, val, (int)strlen(val))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "factory_reset needs token");
        return ESP_OK;
    }
    send_state(req);
    return ESP_OK;
}

// Set the clock from a client (phone/PC): /api/settime?epoch=<unix_utc>[&tz=<POSIX>].
// Optionally sets the POSIX TZ first, then the system clock, the RTC, and the
// persisted epoch. Works over GET or POST so the little JS on the page is simple.
static esp_err_t settime_req(httpd_req_t *req)
{
    char es[24], tz[TZ_STRLEN];
    if (!query_param(req, "epoch", es, sizeof(es))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing epoch");
        return ESP_OK;
    }
    long long epoch = atoll(es);
    if (epoch < 1700000000LL) {   // sanity: after 2023-11
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad epoch");
        return ESP_OK;
    }

    if (query_param(req, "tz", tz, sizeof(tz)) && tz[0]) {
        control_apply_kv("tz", tz, (int)strlen(tz));   // persist
        setenv("TZ", tz, 1);                           // apply now for the reply
        tzset();
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    if (rtc_present()) rtc_set_epoch((int64_t)epoch);   // RTC holds UTC
    settings_persist_epoch((int64_t)epoch);
    ESP_LOGI(TAG, "clock set from client: epoch %lld", epoch);

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

// Give up on a stalled upload after this many back-to-back recv timeouts
// (each = OTA_RECV_TIMEOUT_S). ~30 s total before we abort and recover.
#define OTA_RECV_TIMEOUT_S    10
#define OTA_MAX_RECV_STALLS    3

// Firmware OTA: the raw firmware.bin is streamed in the POST body, written to
// the inactive OTA slot, and the device reboots into it. Runs on the httpd
// worker task (8 KB stack, set below) — the 50 ms display loop is untouched. A
// failed/aborted or stalled upload leaves the running slot bootable (set_boot
// only on success) and un-blanks the display. No auth here: same trust model
// as the rest of this LAN-only UI.
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA -> %s (%d bytes incoming)", part->label, req->content_len);

    // Blank the tubes for the whole flash: the writes below stall the CPU/cache
    // in bursts, which otherwise makes the multiplexed display glitch badly.
    // Cleared again only on failure — on success we reboot.
    nixie_hold_off(true);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        nixie_hold_off(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    char buf[2048];
    int remaining = req->content_len;
    int stalls = 0;   // consecutive recv timeouts; bounded so a half-open
                      // upload can't spin here forever holding the display blank
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            // Each timeout is recv_wait_timeout seconds (set in webui_start).
            // Give up after a handful so a dropped Wi-Fi upload aborts cleanly
            // and the device stays on the current firmware instead of hanging.
            if (++stalls > OTA_MAX_RECV_STALLS) {
                esp_ota_abort(ota);
                nixie_hold_off(false);
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "upload stalled");
                return ESP_OK;
            }
            continue;
        }
        stalls = 0;
        if (r <= 0) {
            esp_ota_abort(ota);
            nixie_hold_off(false);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv error");
            return ESP_OK;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            esp_ota_abort(ota);
            nixie_hold_off(false);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_OK;
        }
        remaining -= r;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        nixie_hold_off(false);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            err == ESP_ERR_OTA_VALIDATE_FAILED ? "bad image" : esp_err_to_name(err));
        return ESP_OK;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        nixie_hold_off(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "OTA complete, rebooting into %s", part->label);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the response flush before we reboot
    esp_restart();
    return ESP_OK;
}

// Recent device log (RAM ring buffer from weblog). Dumped into a heap buffer so
// the big copy stays off the httpd task stack.
static esp_err_t log_get(httpd_req_t *req)
{
    size_t cap = weblog_capacity() + 1;
    char *buf = malloc(cap);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t len = weblog_dump(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// Discovered Shelly BLU (BTHome) devices for the temperature picker.
static esp_err_t blescan_get(httpd_req_t *req)
{
    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t len = blescan_devices_json(buf, 2048);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// Shelly BLE time status for the Clock & time panel (heap buffer, off the stack).
static esp_err_t shellytime_get(httpd_req_t *req)
{
    char *buf = malloc(1536);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t len = shellytime_status_json(buf, 1536);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

void webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // Browsers hold idle keep-alive sockets for minutes. The LRU purge evicts the
    // oldest once max_open_sockets is reached, which only works if lwIP still has
    // a free socket for the new client at that point (CONFIG_LWIP_MAX_SOCKETS=16 >
    // 7 + 3 internal + MQTT/SNTP/DNS). At lwIP's default 10 the web UI locked out.
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    // The state handler builds a ~1.7 KB JSON buffer plus the DS-sensor list on
    // the stack; the default 4 KB httpd task stack overflows. Give it room.
    cfg.stack_size = 8192;
    // Bound each blocking socket recv so a dropped OTA upload times out (and the
    // handler can abort) instead of the worker task blocking indefinitely.
    cfg.recv_wait_timeout = OTA_RECV_TIMEOUT_S;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/state",  .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/set",    .method = HTTP_POST, .handler = set_post },
        { .uri = "/api/set",    .method = HTTP_GET,  .handler = set_get },
        { .uri = "/api/action", .method = HTTP_POST, .handler = action_post },
        { .uri = "/api/action", .method = HTTP_GET,  .handler = action_get },
        { .uri = "/api/settime",.method = HTTP_POST, .handler = settime_req },
        { .uri = "/api/settime",.method = HTTP_GET,  .handler = settime_req },
        { .uri = "/api/net",    .method = HTTP_GET,  .handler = net_get },
        { .uri = "/api/netset", .method = HTTP_POST, .handler = netset_post },
        { .uri = "/api/ota",    .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/log",    .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/blescan",.method = HTTP_GET,  .handler = blescan_get },
        { .uri = "/api/shellytime", .method = HTTP_GET, .handler = shellytime_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(srv, &routes[i]);

    ESP_LOGI(TAG, "web UI started on :80");
}
