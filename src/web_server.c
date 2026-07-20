#include "web_server.h"
#include "app_config.h"
#include "audio_pipeline.h"
#include "rtsp_server.h"
#include "wifi_manager.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "ota_update.h"

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "web";

// ---------------------------------------------------------------------------
// HTML page — %% = literal %, format args listed in root_get_handler()
// ---------------------------------------------------------------------------
static const char *s_html =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Warbler32</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,sans-serif;background:#111827;color:#e5e7eb;margin:0;padding:16px}"
    "h1{color:#60a5fa;margin:0 0 4px}"
    ".sub{color:#6b7280;font-size:13px;margin:0 0 20px}"
    ".url{color:#34d399;font-family:monospace}"
    ".card{background:#1f2937;border-radius:8px;padding:16px;margin-bottom:16px}"
    "h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:#9ca3af;margin:0 0 12px}"
    "label{font-size:13px;color:#9ca3af;display:block;margin-bottom:4px}"
    "input,select{width:100%%;background:#374151;border:1px solid #4b5563;"
    "color:#f3f4f6;padding:8px 10px;border-radius:6px;font-size:14px;margin-bottom:12px}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
    ".val{color:#60a5fa;font-size:12px}"
    ".dim{opacity:.4}"
    ".tip{position:relative;cursor:help}"
    ".tip::after{content:attr(data-tip);position:absolute;left:0;top:100%%;margin-top:2px;"
    "background:#374151;border:1px solid #4b5563;color:#e5e7eb;font-size:11px;"
    "padding:6px 8px;border-radius:4px;width:220px;z-index:10;display:none;line-height:1.4}"
    ".tip:hover::after{display:block}"
    "button{width:100%%;background:#3b82f6;color:#fff;border:none;"
    "padding:12px;border-radius:6px;font-size:15px;font-weight:600;cursor:pointer;margin-top:4px}"
    "button:hover{background:#2563eb}"
    "</style></head><body>"
    "<h1>Warbler32</h1>"
    "%s"
    "<div class=\"card\"><h2>Status</h2>"
    "<div class=\"row\" style=\"grid-template-columns:1fr 1fr 1fr;row-gap:10px\">"
    "<div><label>Uptime</label><span class=\"val\" id=\"stUp\">&ndash;</span></div>"
    "<div><label>WiFi Signal</label><span class=\"val\" id=\"stWifi\">&ndash;</span></div>"
    "<div><label>Free Memory</label><span class=\"val\" id=\"stHeap\">&ndash;</span></div>"
    "<div><label>RTSP Clients</label><span class=\"val\" id=\"stCli\">&ndash;</span></div>"
    "<div><label>Streaming</label><span class=\"val\" id=\"stStr\">&ndash;</span></div>"
    "<div><label>Audio Drops</label><span class=\"val\" id=\"stOvr\">&ndash;</span></div>"
    "</div></div>"
    "<form method=\"POST\" action=\"/save\">"
    "<div class=\"card\"><h2>Device</h2>"
    "<label class=\"tip\" data-tip=\"Name for this device on your network. Becomes the mDNS address, e.g. 'warbler32' -> http://warbler32.local/. Letters, numbers, and hyphens only.\">Name</label>"
    "<input name=\"device_name\" value=\"%s\" autocomplete=\"off\" spellcheck=\"false\" maxlength=\"31\">"
    "</div>"
    "<div class=\"card\"><h2>WiFi</h2>"
    "<label class=\"tip\" data-tip=\"Your WiFi network name. Change this to move the device to a different network. Takes effect after reboot.\">SSID</label>"
    "<input name=\"ssid\" value=\"%s\" autocomplete=\"off\" spellcheck=\"false\">"
    "<label class=\"tip\" data-tip=\"WiFi password. Leave blank for open networks. Stored in device flash.\">Password</label>"
    "<input name=\"password\" type=\"password\" value=\"%s\" autocomplete=\"new-password\">"
    "</div>"
    "<div class=\"card\"><h2>Audio</h2>"
    "<label class=\"tip\" data-tip=\"Which microphone to capture from. INMP441 is the wired I2S mic. USB Microphone requires a UAC-class mic plugged into the native USB port, detected at boot.\">Input Source</label>"
    "<select name=\"audio_source\" id=\"asrc\" onchange=\"toggleGainShift()\">"
    "<option value=\"0\"%s>INMP441 (I2S)</option>"
    "<option value=\"1\"%s>USB Microphone</option>"
    "</select>"
    "<label class=\"tip\" data-tip=\"Audio capture frequency. Higher = better quality but more CPU load. 16 kHz is enough for BirdNET-Go; 48 kHz is full studio quality.\">Sample Rate</label>"
    "<select name=\"sample_rate\">"
    "<option value=\"8000\"%s>8 kHz</option>"
    "<option value=\"16000\"%s>16 kHz</option>"
    "<option value=\"22050\"%s>22.05 kHz</option>"
    "<option value=\"32000\"%s>32 kHz</option>"
    "<option value=\"44100\"%s>44.1 kHz</option>"
    "<option value=\"48000\"%s>48 kHz</option>"
    "</select>"
    "<div class=\"row\">"
    "<div id=\"gsWrap\"><label class=\"tip\" data-tip=\"Right-shifts the 32-bit mic sample to fit 16 bits. Lower = louder. 12 = +12 dB above unity. Raise this if the audio clips or distorts. Only applies to the INMP441 (I2S) - no effect on a USB microphone.\">Gain Shift &nbsp;<span class=\"val\" id=\"sv\">%d</span></label>"
    "<input type=\"range\" name=\"gain_shift\" id=\"gsIn\" min=\"8\" max=\"20\" value=\"%d\""
    " oninput=\"sv.textContent=this.value\"></div>"
    "<div><label class=\"tip\" data-tip=\"Digital gain multiplier. 1 = no extra gain, 8 = +18 dB, 32 = +30 dB. High settings also amplify hiss/noise, not just the signal - for a USB mic, try the hardware volume boost first. Reduce if audio clips.\">Gain Mult &nbsp;<span class=\"val\" id=\"mv\">%d</span></label>"
    "<input type=\"range\" name=\"gain_mult\" min=\"1\" max=\"32\" value=\"%d\""
    " oninput=\"mv.textContent=this.value\"></div>"
    "<div class=\"row\">"
    "<div><label class=\"tip\" data-tip=\"High-pass filter cutoff. 0 = off. Removes DC offset and low-frequency rumble below this frequency. Try 80-200 Hz outdoors to cut wind noise.\">HPF Cutoff &nbsp;<span class=\"val\" id=\"hpfv\">%d</span> Hz</label>"
    "<input type=\"range\" name=\"hpf_freq\" min=\"0\" max=\"1000\" step=\"10\" value=\"%d\""
    " oninput=\"hpfv.textContent=this.value==0?'Off':this.value\"></div>"
    "<div><label class=\"tip\" data-tip=\"Mutes output when audio falls below this level. Reduces BirdNET-Go CPU during silence. 0 = disabled, 100-500 = typical range.\">Noise Gate &nbsp;<span class=\"val\" id=\"ngv\">%d</span></label>"
    "<input type=\"range\" name=\"noise_gate\" min=\"0\" max=\"2000\" value=\"%d\""
    " oninput=\"ngv.textContent=this.value\"></div>"
    "</div></div>"
    "<div class=\"card\"><h2>LED</h2>"
    "<label class=\"tip\" data-tip=\"Status LED brightness. 0 = off, 255 = maximum. Values of 20-50 work well indoors. Blue = connecting, solid green = connected, blinking green = streaming, orange = WiFi failed, red = setup mode.\">Brightness &nbsp;<span class=\"val\" id=\"bv\">%d</span></label>"
    "<input type=\"range\" name=\"led_brightness\" min=\"0\" max=\"255\" value=\"%d\""
    " oninput=\"bv.textContent=this.value\">"
    "</div>"
    "<button type=\"submit\">Save</button>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 0;text-align:center\">"
    "Audio &amp; LED changes apply instantly; name, WiFi, input source, and "
    "sample rate changes reboot the device.</p>"
    "</form>"
    "<div class=\"card\" style=\"margin-top:16px\"><h2>Audio Monitor</h2>"
    "<canvas id=\"lv\" width=\"600\" height=\"72\""
    " style=\"width:100%%;height:72px;border-radius:4px;background:#111827\"></canvas>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 4px\">Peak level &mdash; green &lt;55%%, amber &lt;80%%, red = clipping.</p>"
    "<button type=\"button\" onclick=\"toggleMon(this)\""
    " style=\"background:#374151;width:auto;padding:8px 16px;font-size:13px;margin:0\">Start Monitor</button>"
    "</div>"
    "<div class=\"card\" style=\"margin-top:16px\"><h2>Firmware</h2>"
    "<p style=\"font-size:12px;color:#6b7280;margin:0 0 10px\">Running <span style=\"color:#9ca3af\">%s</span>"
    " (%s board) &middot; built %s</p>"
    "<button type=\"button\" id=\"chkbtn\" onclick=\"doChk()\""
    " style=\"background:#374151;margin:0 0 8px\">Check for Updates</button>"
    "<p id=\"chkst\" style=\"font-size:12px;color:#6b7280;margin:0 0 8px\"></p>"
    "<button type=\"button\" id=\"instbtn\" onclick=\"doInst()\""
    " style=\"display:none;margin:0 0 8px\">Install</button>"
    "<div id=\"obw\" style=\"display:none;background:#374151;border-radius:4px;height:8px;margin-bottom:8px\">"
    "<div id=\"ob\" style=\"background:#3b82f6;height:8px;border-radius:4px;width:0\"></div></div>"
    "<p id=\"ost\" style=\"font-size:12px;color:#6b7280;margin:0 0 8px\"></p>"
    "<details style=\"margin-top:4px\"><summary style=\"font-size:12px;color:#9ca3af;"
    "cursor:pointer\">Manual update (.bin upload)</summary>"
    "<p style=\"font-size:12px;color:#6b7280;margin:8px 0 10px\">Upload a locally built "
    "<code>firmware.bin</code> &mdash; the device reboots into it when the install finishes.</p>"
    "<input type=\"file\" id=\"fw\" accept=\".bin\">"
    "<button type=\"button\" id=\"obtn\" onclick=\"doOta()\""
    " style=\"background:#374151\">Upload &amp; Install</button>"
    "</details>"
    "</div>"
    "<div class=\"card\" style=\"margin-top:16px\"><h2>Danger Zone</h2>"
    "<p style=\"font-size:12px;color:#6b7280;margin:0 0 10px\">Erases saved WiFi and audio "
    "settings and reboots into setup mode (broadcasting \"" WIFI_AP_SSID "\").</p>"
    "<form method=\"POST\" action=\"/reset\" onsubmit=\"return confirm('Factory reset? This erases "
    "saved WiFi and audio settings.');\">"
    "<button type=\"submit\" style=\"background:#dc2626\">Factory Reset</button>"
    "</form>"
    "</div>"
    "<script>"
    "(function(){"
    "var c=document.getElementById('lv'),x=c.getContext('2d'),h=[],t=null;"
    "function draw(){"
    "var W=c.width,H=c.height,bw=Math.floor(W/50)-1;"
    "x.fillStyle='#111827';x.fillRect(0,0,W,H);"
    "for(var i=0;i<h.length;i++){"
    "x.fillStyle=h[i]>80?'#ef4444':h[i]>55?'#f59e0b':'#34d399';"
    "x.fillRect(i*(bw+1),H-h[i]*H/100,bw,h[i]*H/100);"
    "}}"
    "window.toggleMon=function(b){"
    "if(t){clearInterval(t);t=null;h=[];draw();b.textContent='Start Monitor';}"
    "else{t=setInterval(function(){"
    "fetch('/level').then(function(r){return r.json();})"
    ".then(function(j){h.push(j.p);if(h.length>50)h.shift();draw();});"
    "},150);b.textContent='Stop Monitor';}"
    "};"
    "function stFmt(s){"
    "var d=Math.floor(s/86400),h=Math.floor(s%%86400/3600),m=Math.floor(s%%3600/60);"
    "return d>0?d+'d '+h+'h':h>0?h+'h '+m+'m':m+'m '+s%%60+'s';"
    "}"
    "function stPoll(){"
    "fetch('/status').then(function(r){return r.json();}).then(function(j){"
    "document.getElementById('stUp').textContent=stFmt(j.uptime);"
    "document.getElementById('stWifi').textContent=j.rssi?j.rssi+' dBm':'\\u2013';"
    "document.getElementById('stHeap').textContent=Math.round(j.heap/1024)+' KB';"
    "document.getElementById('stCli').textContent=j.clients+' / '+j.max_clients;"
    "document.getElementById('stStr').textContent=j.streaming?j.streaming+' active':'no';"
    "document.getElementById('stOvr').textContent=j.overruns;"
    "}).catch(function(){});"
    "}"
    "setInterval(stPoll,2000);stPoll();"
    "var $=function(i){return document.getElementById(i);};"
    "window.doChk=function(){"
    "var b=$('chkbtn'),st=$('chkst');"
    "b.disabled=true;st.textContent='Checking\\u2026';$('instbtn').style.display='none';"
    "fetch('/update/check',{method:'POST'}).then(function(r){return r.json();})"
    ".then(function(j){"
    "b.disabled=false;"
    "if(j.error){st.textContent=j.error;return;}"
    "if(j.available){"
    "st.textContent='Update available: '+j.latest+' ('+Math.round(j.size/1024)+' KB, '+j.variant+' build)';"
    "var ib=$('instbtn');ib.textContent='Install '+j.latest;ib.style.display='block';"
    "}else{st.textContent='Up to date ('+j.current+').';}"
    "}).catch(function(){b.disabled=false;st.textContent='Check failed \\u2014 connection error.';});"
    "};"
    "window.doInst=function(){"
    "var ib=$('instbtn'),st=$('chkst'),bw=$('obw'),bar=$('ob');"
    "if(!confirm(ib.textContent+'? The device reboots when the install finishes.'))return;"
    "fetch('/update/install',{method:'POST'}).then(function(r){return r.json();})"
    ".then(function(j){"
    "if(j.error){st.textContent=j.error;return;}"
    "ib.disabled=true;$('chkbtn').disabled=true;bw.style.display='block';"
    "var fails=0;"
    "var t=setInterval(function(){"
    "fetch('/update/progress').then(function(r){return r.json();}).then(function(p){"
    "fails=0;"
    "if(p.state=='downloading'){bar.style.width=p.pct+'%%';st.textContent='Downloading\\u2026 '+p.pct+'%%';}"
    "else if(p.state=='verifying'){bar.style.width='100%%';st.textContent='Verifying & installing\\u2026';}"
    "else if(p.state=='rebooting'){clearInterval(t);st.textContent='Installed \\u2014 rebooting\\u2026';"
    "setTimeout(function(){location.reload()},12000);}"
    "else if(p.state=='error'){clearInterval(t);st.textContent='Failed: '+p.msg;"
    "ib.disabled=false;$('chkbtn').disabled=false;}"
    "}).catch(function(){"
    "if(++fails>=4){clearInterval(t);st.textContent='Installed \\u2014 rebooting\\u2026';"
    "setTimeout(function(){location.reload()},10000);}"
    "});"
    "},1000);"
    "});"
    "};"
    "window.doOta=function(){"
    "var f=document.getElementById('fw').files[0];"
    "if(!f){alert('Choose a firmware .bin file first.');return;}"
    "if(!confirm('Install '+f.name+' ('+Math.round(f.size/1024)+' KB)? "
    "The device reboots when the install finishes.'))return;"
    "var b=document.getElementById('obtn'),st=document.getElementById('ost'),"
    "bw=document.getElementById('obw'),bar=document.getElementById('ob');"
    "b.disabled=true;bw.style.display='block';"
    "var x=new XMLHttpRequest();"
    "x.open('POST','/ota');"
    "x.timeout=300000;"
    "x.upload.onprogress=function(e){"
    "if(!e.lengthComputable)return;"
    "var p=Math.round(e.loaded*100/e.total);bar.style.width=p+'%%';"
    "st.textContent=e.loaded<e.total?'Uploading\\u2026 '+p+'%%':'Verifying & installing\\u2026';"
    "};"
    "x.onload=function(){"
    "if(x.status==200){st.textContent='Installed \\u2014 rebooting\\u2026';"
    "setTimeout(function(){location.reload()},12000);}"
    "else{st.textContent='Failed: '+x.responseText;b.disabled=false;}"
    "};"
    "x.onerror=x.ontimeout=function(){"
    "st.textContent='Upload failed \\u2014 connection error.';b.disabled=false;};"
    "x.send(f);"
    "};"
    "window.toggleGainShift=function(){"
    "var usb=document.getElementById('asrc').value=='1';"
    "document.getElementById('gsIn').disabled=usb;"
    "document.getElementById('gsWrap').classList.toggle('dim',usb);"
    "};"
    "toggleGainShift();"
    "})();"
    "</script>"
    "</body></html>";

// ---------------------------------------------------------------------------
// POST body helpers
// ---------------------------------------------------------------------------
static void url_decode(char *s)
{
    char *w = s, *r = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' '; r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], '\0'};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void get_field(const char *body, const char *key, char *out, size_t n)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(needle);
    const char *e = strchr(p, '&');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    url_decode(out);
}

// Escapes &, <, >, ", ' for safe embedding into HTML attribute/text context.
// Truncates at `outsz` without splitting an entity if it wouldn't fit whole.
static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o < outsz - 1; i++) {
        const char *rep = NULL;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= outsz) break;
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// GET / — serve config page
// ---------------------------------------------------------------------------
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char status_line[256];
    if (wifi_manager_is_ap_mode()) {
        snprintf(status_line, sizeof(status_line),
            "<p class=\"sub\" style=\"color:#f59e0b\">Setup Mode &mdash; enter your WiFi "
            "network below, then Save.</p>");
    } else {
        char ip[16] = "0.0.0.0";
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        snprintf(status_line, sizeof(status_line),
            "<p class=\"sub\">Stream: <span class=\"url\">rtsp://%s/audio</span></p>", ip);
    }

    // Escape before reflecting into the page — SSID/password can contain
    // almost any printable character (unlike device_name, which is already
    // restricted to alnum+hyphen at save time), so this is output encoding,
    // not input filtering.
    char name_esc[200], ssid_esc[400], pass_esc[400];
    html_escape(g_config.device_name,   name_esc, sizeof(name_esc));
    html_escape(g_config.wifi_ssid,     ssid_esc, sizeof(ssid_esc));
    html_escape(g_config.wifi_password, pass_esc, sizeof(pass_esc));

    const esp_app_desc_t *app = esp_app_get_description();

    const size_t bufsz = 16384;
    char *buf = malloc(bufsz);
    if (!buf) return ESP_ERR_NO_MEM;

    int len = snprintf(buf, bufsz, s_html,
        status_line,
        name_esc,
        ssid_esc,
        pass_esc,
        g_config.audio_source == AUDIO_SOURCE_I2S ? " selected" : "",
        g_config.audio_source == AUDIO_SOURCE_USB ? " selected" : "",
        g_config.sample_rate ==  8000 ? " selected" : "",
        g_config.sample_rate == 16000 ? " selected" : "",
        g_config.sample_rate == 22050 ? " selected" : "",
        g_config.sample_rate == 32000 ? " selected" : "",
        g_config.sample_rate == 44100 ? " selected" : "",
        g_config.sample_rate == 48000 ? " selected" : "",
        (int)g_config.gain_shift,  (int)g_config.gain_shift,
        (int)g_config.gain_mult,   (int)g_config.gain_mult,
        (int)g_config.hpf_freq, (int)g_config.hpf_freq,
        (int)g_config.noise_gate, (int)g_config.noise_gate,
        (int)g_config.led_brightness, (int)g_config.led_brightness,
        app->version, ota_board_variant(), app->date);

    // snprintf returns the would-be length: if the page outgrows the buffer,
    // send only what actually fits rather than trailing heap garbage.
    if (len >= (int)bufsz) {
        ESP_LOGE(TAG, "config page truncated (%d >= %u) — grow bufsz", len, (unsigned)bufsz);
        len = bufsz - 1;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /save — update config, reboot
// ---------------------------------------------------------------------------
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    int body_len = req->content_len;
    if (body_len <= 0 || body_len > 511) body_len = 511;

    char *body = malloc(body_len + 1);
    if (!body) return ESP_ERR_NO_MEM;

    int received = httpd_req_recv(req, body, body_len);
    if (received <= 0) { free(body); return ESP_FAIL; }
    body[received] = '\0';

    // Snapshot the fields that require a reboot to take effect (mDNS/WiFi
    // re-init, mic driver + I2S clock re-init). Everything else — gains,
    // HPF, noise gate, LED brightness — is read from g_config live by the
    // audio/LED code and applies the moment the struct is updated.
    char     old_name[sizeof(g_config.device_name)];
    char     old_ssid[sizeof(g_config.wifi_ssid)];
    char     old_pass[sizeof(g_config.wifi_password)];
    uint8_t  old_source = g_config.audio_source;
    uint32_t old_rate   = g_config.sample_rate;
    strlcpy(old_name, g_config.device_name,   sizeof(old_name));
    strlcpy(old_ssid, g_config.wifi_ssid,     sizeof(old_ssid));
    strlcpy(old_pass, g_config.wifi_password, sizeof(old_pass));

    char val[128];

    get_field(body, "device_name", val, sizeof(val));
    if (val[0]) {
        char clean[sizeof(g_config.device_name)];
        size_t n = 0;
        for (size_t i = 0; val[i] != '\0' && n < sizeof(clean) - 1; i++) {
            char c = val[i];
            if (isalnum((unsigned char)c) || c == '-') clean[n++] = c;
        }
        clean[n] = '\0';
        strlcpy(g_config.device_name, n ? clean : DEVICE_NAME_DEFAULT, sizeof(g_config.device_name));
    }

    get_field(body, "ssid", val, sizeof(val));
    if (val[0]) strlcpy(g_config.wifi_ssid, val, sizeof(g_config.wifi_ssid));

    get_field(body, "password", val, sizeof(val));
    strlcpy(g_config.wifi_password, val, sizeof(g_config.wifi_password));

    get_field(body, "audio_source", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v == AUDIO_SOURCE_I2S || v == AUDIO_SOURCE_USB)
            g_config.audio_source = (uint8_t)v;
    }

    get_field(body, "sample_rate", val, sizeof(val));
    if (val[0]) {
        uint32_t sr = (uint32_t)atoi(val);
        if (sr == 8000 || sr == 16000 || sr == 22050 ||
            sr == 32000 || sr == 44100 || sr == 48000)
            g_config.sample_rate = sr;
    }

    get_field(body, "gain_shift", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 8 && v <= 20) g_config.gain_shift = (uint8_t)v;
    }

    get_field(body, "gain_mult", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 1 && v <= 32) g_config.gain_mult = (uint8_t)v;
    }

    get_field(body, "led_brightness", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 0 && v <= 255) g_config.led_brightness = (uint8_t)v;
    }

    get_field(body, "hpf_freq", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 0 && v <= 1000) g_config.hpf_freq = (uint16_t)v;
    }

    get_field(body, "noise_gate", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 0 && v <= 2000) g_config.noise_gate = (uint16_t)v;
    }

    free(body);
    app_config_save();

    bool reboot_needed =
        strcmp(old_name, g_config.device_name)   != 0 ||
        strcmp(old_ssid, g_config.wifi_ssid)     != 0 ||
        strcmp(old_pass, g_config.wifi_password) != 0 ||
        old_source != g_config.audio_source ||
        old_rate   != g_config.sample_rate;

    httpd_resp_set_type(req, "text/html");

    if (reboot_needed) {
        static const char resp[] =
            "<!DOCTYPE html><html><head>"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Warbler32</title></head>"
            "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
            "text-align:center;padding:60px 16px\">"
            "<h2 style=\"color:#34d399\">Saved!</h2>"
            "<p>Device is rebooting&hellip;</p>"
            "<p style=\"color:#6b7280;font-size:13px\">Page will reload in 8 seconds.</p>"
            "<script>setTimeout(()=>{location.href='/'},8000)</script>"
            "</body></html>";
        httpd_resp_send(req, resp, strlen(resp));
        xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    } else {
        ESP_LOGI(TAG, "config saved, applied live (no reboot)");
        static const char resp[] =
            "<!DOCTYPE html><html><head>"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Warbler32</title></head>"
            "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
            "text-align:center;padding:60px 16px\">"
            "<h2 style=\"color:#34d399\">Saved!</h2>"
            "<p>Settings applied &mdash; no reboot needed.</p>"
            "<script>setTimeout(()=>{location.href='/'},1200)</script>"
            "</body></html>";
        httpd_resp_send(req, resp, strlen(resp));
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /reset — wipe saved config, reboot into setup mode
// ---------------------------------------------------------------------------
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    app_config_factory_reset();

    static const char resp[] =
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Warbler32</title></head>"
        "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
        "text-align:center;padding:60px 16px\">"
        "<h2 style=\"color:#f59e0b\">Factory Reset</h2>"
        "<p>Settings erased. The device is rebooting into setup mode.</p>"
        "<p style=\"color:#6b7280;font-size:13px\">Connect to \"" WIFI_AP_SSID "\" and browse to "
        "192.168.4.1 to set it up again.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /ota — stream a manually uploaded firmware.bin into the OTA engine
// ---------------------------------------------------------------------------
#define OTA_BUF_SIZE 4096

static esp_err_t ota_fail(httpd_req_t *req, char *buf,
                          httpd_err_code_t code, const char *msg)
{
    ESP_LOGE(TAG, "OTA upload rejected: %s", msg);
    ota_engine_abort();
    free(buf);
    httpd_resp_send_err(req, code, msg);
    return ESP_FAIL;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const char *emsg = "Upload failed";
    int total = req->content_len;

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf)
        return ota_fail(req, NULL, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Out of memory");

    if (ota_engine_begin((size_t)(total > 0 ? total : 0), &emsg) != ESP_OK)
        return ota_fail(req, buf, HTTPD_400_BAD_REQUEST, emsg);

    ESP_LOGI(TAG, "OTA upload: %d bytes", total);

    int remaining = total, timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf,
                               remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0)
            return ota_fail(req, buf, HTTPD_400_BAD_REQUEST, "Upload interrupted");
        timeouts = 0;

        if (ota_engine_feed((const uint8_t *)buf, (size_t)n, &emsg) != ESP_OK) {
            // feed already aborted the engine
            ESP_LOGE(TAG, "OTA upload rejected: %s", emsg);
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, emsg);
            return ESP_FAIL;
        }
        remaining -= n;
    }
    free(buf);

    if (ota_engine_finish(&emsg) != ESP_OK) {
        ESP_LOGE(TAG, "OTA upload rejected: %s", emsg);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, emsg);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GitHub update endpoints
// ---------------------------------------------------------------------------
static esp_err_t update_check_post_handler(httpd_req_t *req)
{
    ota_check_result_t res;
    const char *emsg = "";
    char json[320];
    int len;

    if (ota_github_check(&res, &emsg) != ESP_OK) {
        len = snprintf(json, sizeof(json), "{\"error\":\"%s\"}", emsg);
    } else {
        len = snprintf(json, sizeof(json),
            "{\"current\":\"%s\",\"latest\":\"%s\",\"available\":%s,"
            "\"size\":%u,\"variant\":\"%s\"}",
            res.current, res.latest, res.available ? "true" : "false",
            (unsigned)res.size, ota_board_variant());
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t update_install_post_handler(httpd_req_t *req)
{
    const char *emsg = "";
    char json[160];
    int len;

    if (ota_github_install(&emsg) != ESP_OK)
        len = snprintf(json, sizeof(json), "{\"error\":\"%s\"}", emsg);
    else
        len = snprintf(json, sizeof(json), "{\"ok\":true}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t update_progress_get_handler(httpd_req_t *req)
{
    ota_progress_t p;
    ota_github_progress(&p);

    char json[192];
    int len = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"pct\":%d,\"msg\":\"%s\"}", p.state, p.pct, p.msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /level — audio peak level for the browser monitor
// ---------------------------------------------------------------------------
static esp_err_t level_get_handler(httpd_req_t *req)
{
    char json[16];
    int pct = audio_pipeline_get_peak_pct();
    snprintf(json, sizeof(json), "{\"p\":%d}", pct);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /status — device diagnostics for the web UI (and anything else)
// ---------------------------------------------------------------------------
static esp_err_t status_get_handler(httpd_req_t *req)
{
    int rssi = 0;
    if (!wifi_manager_is_ap_mode()) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    }

    char json[320];
    int len = snprintf(json, sizeof(json),
        "{\"uptime\":%lld,\"heap\":%u,\"heap_min\":%u,\"psram\":%u,"
        "\"rssi\":%d,\"clients\":%d,\"max_clients\":%d,\"streaming\":%d,"
        "\"overruns\":%u,\"peak\":%d,\"version\":\"%s\",\"variant\":\"%s\"}",
        esp_timer_get_time() / 1000000,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        rssi,
        rtsp_server_client_count(), PIPELINE_MAX_READERS,
        rtsp_server_streaming_count(),
        (unsigned)audio_pipeline_get_overruns(),
        audio_pipeline_get_peak_pct(),
        esp_app_get_description()->version, ota_board_variant());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 12;
    cfg.stack_size        = 12288;  // TLS handshake runs in-handler for /update/check

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    static const httpd_uri_t get_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
    };
    static const httpd_uri_t post_save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    static const httpd_uri_t get_level = {
        .uri = "/level", .method = HTTP_GET, .handler = level_get_handler,
    };
    static const httpd_uri_t post_reset = {
        .uri = "/reset", .method = HTTP_POST, .handler = reset_post_handler,
    };
    static const httpd_uri_t post_ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler,
    };
    static const httpd_uri_t get_status = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t post_upd_check = {
        .uri = "/update/check", .method = HTTP_POST, .handler = update_check_post_handler,
    };
    static const httpd_uri_t post_upd_install = {
        .uri = "/update/install", .method = HTTP_POST, .handler = update_install_post_handler,
    };
    static const httpd_uri_t get_upd_progress = {
        .uri = "/update/progress", .method = HTTP_GET, .handler = update_progress_get_handler,
    };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);
    httpd_register_uri_handler(server, &get_level);
    httpd_register_uri_handler(server, &post_reset);
    httpd_register_uri_handler(server, &post_ota);
    httpd_register_uri_handler(server, &get_status);
    httpd_register_uri_handler(server, &post_upd_check);
    httpd_register_uri_handler(server, &post_upd_install);
    httpd_register_uri_handler(server, &get_upd_progress);

    if (wifi_manager_is_ap_mode()) {
        ESP_LOGI(TAG, "config UI: http://192.168.4.1/ (connect to \"%s\" first)", WIFI_AP_SSID);
    } else {
        char ip[16] = "0.0.0.0";
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "config UI: http://%s/", ip);
    }
    return ESP_OK;
}
