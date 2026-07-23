#include "web_server.h"
#include "app_config.h"
#include "audio_pipeline.h"
#include "mic_health.h"
#include "battery_monitor.h"
#include "pipeline_watchdog.h"
#include "rtsp_server.h"
#include "wifi_manager.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "ota_update.h"

#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdatomic.h>

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
    ".battIcon{display:none;flex-direction:column-reverse;gap:1px;"
    "width:16px;height:30px;border:2px solid #6b7280;border-radius:2px;"
    "padding:2px;position:relative}"
    ".battIcon::before{content:'';position:absolute;top:-5px;left:50%%;"
    "transform:translateX(-50%%);width:8px;height:3px;background:#6b7280;"
    "border-radius:1px 1px 0 0}"
    ".battIcon .bar{flex:1;background:#374151;border-radius:1px}"
    ".battIcon .bar.filled{background:#34d399}"
    ".battIcon.low .bar.filled{background:#f87171}"
    ".dim{opacity:.4}"
    ".tip{position:relative;cursor:help}"
    ".tip::after{content:attr(data-tip);position:absolute;left:0;top:100%%;margin-top:2px;"
    "background:#374151;border:1px solid #4b5563;color:#e5e7eb;font-size:11px;"
    "padding:6px 8px;border-radius:4px;width:220px;z-index:10;display:none;line-height:1.4}"
    ".tip:hover::after{display:block}"
    "button{width:100%%;background:#3b82f6;color:#fff;border:none;"
    "padding:12px;border-radius:6px;font-size:15px;font-weight:600;cursor:pointer;margin-top:4px}"
    "button:hover{background:#2563eb}"
    ".layout{display:flex;gap:16px;align-items:flex-start}"
    ".tabnav{display:flex;flex-direction:column;gap:4px;width:170px;flex-shrink:0}"
    ".tabbtn{background:#1f2937;color:#9ca3af;border:1px solid #374151;text-align:left;"
    "padding:10px 12px;border-radius:6px;font-size:13px;font-weight:400;cursor:pointer;"
    "width:100%%;margin-top:0}"
    ".tabbtn:hover{background:#374151}"
    ".tabbtn.active{background:#3b82f6;color:#fff;border-color:#3b82f6}"
    ".tabbtn.active:hover{background:#3b82f6}"
    ".content{flex:1;min-width:0}"
    ".tab-panel{display:none}"
    ".tab-panel.active{display:block}"
    ".statusStrip{display:flex;flex-wrap:wrap;gap:6px 18px;font-size:12px;color:#9ca3af;"
    "background:#1f2937;border-radius:8px;padding:10px 14px;margin-bottom:16px}"
    ".statusStrip b{color:#e5e7eb;font-weight:600}"
    "@media(max-width:700px){"
    ".layout{flex-direction:column}"
    ".tabnav{flex-direction:row;width:100%%;overflow-x:auto;padding-bottom:2px;gap:6px}"
    ".tabbtn{width:auto;white-space:nowrap;flex-shrink:0}"
    "}"
    "</style></head><body>"
    "<div style=\"display:flex;justify-content:space-between;align-items:center\">"
    "<h1>Warbler32</h1>"
    "<div class=\"battIcon\" id=\"battIcon\">"
    "<div class=\"bar\"></div><div class=\"bar\"></div><div class=\"bar\"></div>"
    "<div class=\"bar\"></div><div class=\"bar\"></div><div class=\"bar\"></div>"
    "<div class=\"bar\"></div><div class=\"bar\"></div><div class=\"bar\"></div>"
    "<div class=\"bar\"></div>"
    "</div></div>"
    "%s"
    "<div class=\"layout\">"
    "<nav class=\"tabnav\">"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"dashboard\">Dashboard</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"device\">Device &amp; Network</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"audio\">Audio</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"hardware\">Hardware</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"diagnostics\">Diagnostics</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"firmware\">Firmware</button>"
    "<button type=\"button\" class=\"tabbtn\" data-tab=\"advanced\">Advanced</button>"
    "</nav>"
    "<div class=\"content\">"
    "<div class=\"statusStrip\">"
    "<span>Up <b id=\"stUpM\">&ndash;</b></span>"
    "<span>WiFi <b id=\"stWifiM\">&ndash;</b></span>"
    "<span>Streaming <b id=\"stStrM\">&ndash;</b></span>"
    "<span>Mic <b id=\"stMicM\">&ndash;</b></span>"
    "<span>Watchdog <b id=\"stWdM\">&ndash;</b></span>"
    "<span>Battery <b id=\"stBattM\">&ndash;</b></span>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-dashboard\">"
    "<div class=\"card\"><h2>Status</h2>"
    "<div class=\"row\" style=\"grid-template-columns:1fr 1fr 1fr;row-gap:10px\">"
    "<div><label>Uptime</label><span class=\"val\" id=\"stUp\">&ndash;</span></div>"
    "<div><label>WiFi Signal</label><span class=\"val\" id=\"stWifi\">&ndash;</span></div>"
    "<div><label>Free Memory</label><span class=\"val\" id=\"stHeap\">&ndash;</span></div>"
    "<div><label>RTSP Clients</label><span class=\"val\" id=\"stCli\">&ndash;</span></div>"
    "<div><label>Streaming</label><span class=\"val\" id=\"stStr\">&ndash;</span></div>"
    "<div><label>Audio Drops</label><span class=\"val\" id=\"stOvr\">&ndash;</span></div>"
    "<div><label class=\"tip\" data-tip=\"Samples the I2S driver overwrote at the DMA level before the reader task could pull them out - a lower-level drop than Audio Drops above, which only counts packets lost once they reach the streaming ring buffer. Always 0 on the USB mic.\">DMA Overflows</label><span class=\"val\" id=\"stDma\">&ndash;</span></div>"
    "<div><label class=\"tip\" data-tip=\"Watches the raw mic signal for a flatline (dead mic, broken wire). SILENT means no signal movement for 20+ seconds - the status LED also blinks magenta. A healthy mic's self-noise never trips this, even in a quiet room.\">Mic Health</label><span class=\"val\" id=\"stMic\">&ndash;</span></div>"
    "<div><label class=\"tip\" data-tip=\"Watches whether the audio reader task is producing data at all. STALLED counts up toward an automatic reboot; OFF means the Diagnostics setting below has this disabled.\">Watchdog</label><span class=\"val\" id=\"stWd\">&ndash;</span></div>"
    "<div><label class=\"tip\" data-tip=\"Live voltage from the optional INA219 battery monitor. Shows an em dash if no INA219 is wired up. See the icon next to the page title for an at-a-glance level.\">Battery</label><span class=\"val\" id=\"stBatt\">&ndash;</span></div>"
    "</div></div>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-device\">"
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
    "<label class=\"tip\" data-tip=\"Maximum WiFi transmit power. Lower values reduce range but can also reduce RF noise coupling into nearby mic wiring/power rails if audio sounds noisy despite a stable connection. 20 dBm = full power.\">TX Power &nbsp;<span class=\"val\" id=\"txv\">%d</span> dBm</label>"
    "<input type=\"range\" name=\"tx_power\" min=\"8\" max=\"20\" value=\"%d\""
    " oninput=\"txv.textContent=this.value\">"
    "</div>"
    "<input type=\"hidden\" name=\"tab\" value=\"device\">"
    "<button type=\"submit\">Save</button>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 0;text-align:center\">"
    "TX Power applies instantly; Name, SSID, and Password changes reboot the device.</p>"
    "</form>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-audio\">"
    "<form method=\"POST\" action=\"/save\">"
    "<div class=\"card\"><h2>Audio</h2>"
    "<label class=\"tip\" data-tip=\"Which microphone to capture from. I2S is a wired MEMS mic (pick the model below). USB Microphone requires a UAC-class mic plugged into the native USB port, detected at boot.\">Input Source</label>"
    "<select name=\"audio_source\" id=\"asrc\" onchange=\"toggleGainShift()\">"
    "<option value=\"0\"%s>I2S Microphone</option>"
    "<option value=\"1\"%s>USB Microphone</option>"
    "</select>"
    "<div id=\"mmWrap\"><label class=\"tip\" data-tip=\"Which I2S MEMS mic is wired up. Both use the same pins (L/R or SEL tied to GND), but they need different bus timing. Pick the wrong one and you get silence or heavy distortion. The SPH0645 has a DC offset - enable the high-pass filter to remove it.\">I2S Mic Model</label>"
    "<select name=\"mic_model\" id=\"mmSel\">"
    "<option value=\"0\"%s>INMP441</option>"
    "<option value=\"1\"%s>SPH0645</option>"
    "</select></div>"
    "<label class=\"tip\" data-tip=\"Audio capture frequency. Higher = better quality but more CPU load. 16 kHz is enough for BirdNET-Go; 48 kHz is full studio quality.\">Sample Rate</label>"
    "<select name=\"sample_rate\" id=\"srSel\" onchange=\"drawHpf()\">"
    "<option value=\"8000\"%s>8 kHz</option>"
    "<option value=\"16000\"%s>16 kHz</option>"
    "<option value=\"22050\"%s>22.05 kHz</option>"
    "<option value=\"32000\"%s>32 kHz</option>"
    "<option value=\"44100\"%s>44.1 kHz</option>"
    "<option value=\"48000\"%s>48 kHz</option>"
    "</select>"
    "<div class=\"row\">"
    "<div id=\"gsWrap\"><label class=\"tip\" data-tip=\"Right-shifts the 32-bit mic sample to fit 16 bits. Lower = louder. 12 = +12 dB above unity. Raise this if the audio clips or distorts. Only applies to the I2S mic - no effect on a USB microphone.\">Gain Shift &nbsp;<span class=\"val\" id=\"sv\">%d</span></label>"
    "<input type=\"range\" name=\"gain_shift\" id=\"gsIn\" min=\"8\" max=\"20\" value=\"%d\""
    " oninput=\"sv.textContent=this.value\"></div>"
    "<div><label class=\"tip\" data-tip=\"Digital gain multiplier. 1 = no extra gain, 8 = +18 dB, 32 = +30 dB. High settings also amplify hiss/noise, not just the signal - for a USB mic, try the hardware volume boost first. Reduce if audio clips.\">Gain Mult &nbsp;<span class=\"val\" id=\"mv\">%d</span></label>"
    "<input type=\"range\" name=\"gain_mult\" min=\"1\" max=\"32\" value=\"%d\""
    " oninput=\"mv.textContent=this.value\"></div>"
    "<div class=\"row\">"
    "<div><label class=\"tip\" data-tip=\"High-pass filter cutoff. 0 = off. Removes DC offset and low-frequency rumble below this frequency. Try 80-200 Hz outdoors to cut wind noise.\">HPF Cutoff &nbsp;<span class=\"val\" id=\"hpfv\">%d</span> Hz</label>"
    "<input type=\"range\" name=\"hpf_freq\" id=\"hpfIn\" min=\"0\" max=\"1000\" step=\"10\" value=\"%d\""
    " oninput=\"hpfv.textContent=this.value==0?'Off':this.value;drawHpf()\"></div>"
    "</div>"
    "<div class=\"row\">"
    "<div><label class=\"tip\" data-tip=\"How steeply frequencies below the cutoff roll off. 6 dB/octave is gentle; 24 dB/octave is close to a brick wall - best for killing wind rumble.\">HPF Slope</label>"
    "<select name=\"hpf_slope\" id=\"slopeSel\" onchange=\"drawHpf()\">"
    "<option value=\"1\"%s>6 dB/octave</option>"
    "<option value=\"2\"%s>12 dB/octave</option>"
    "<option value=\"3\"%s>18 dB/octave</option>"
    "<option value=\"4\"%s>24 dB/octave</option>"
    "</select></div>"
    "<div><label class=\"tip\" data-tip=\"Maximum attenuation of the filter. Full removes lows entirely; smaller values only push them down by that many dB, keeping some low-end ambience. 0 dB bypasses the filter.\">HPF Depth &nbsp;<span class=\"val\" id=\"hdv\">%d</span></label>"
    "<input type=\"range\" name=\"hpf_depth\" id=\"depthIn\" min=\"0\" max=\"60\" step=\"3\" value=\"%d\""
    " oninput=\"hdv.textContent=this.value>=60?'Full':this.value+' dB';drawHpf()\"></div>"
    "</div>"
    "</div>"
    "</div>"
    "<label style=\"margin-top:2px\">Filter Response</label>"
    "<canvas id=\"hg\" width=\"600\" height=\"220\""
    " style=\"width:100%%;height:220px;border-radius:4px;background:#111827\"></canvas>"
    "<input type=\"hidden\" name=\"tab\" value=\"audio\">"
    "<button type=\"submit\">Save</button>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 0;text-align:center\">"
    "Gain and filter settings apply instantly; Input Source and Sample Rate changes reboot the device.</p>"
    "</form>"
    "<div class=\"card\" style=\"margin-top:16px\"><h2>Audio Monitor</h2>"
    "<canvas id=\"lv\" width=\"600\" height=\"72\""
    " style=\"width:100%%;height:72px;border-radius:4px;background:#111827\"></canvas>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 4px\">Peak level &mdash; green &lt;55%%, amber &lt;80%%, red = clipping.</p>"
    "<button type=\"button\" onclick=\"toggleMon(this)\""
    " style=\"background:#374151;width:auto;padding:8px 16px;font-size:13px;margin:0\">Start Monitor</button>"
    "<button type=\"button\" id=\"lsnbtn\" onclick=\"toggleListen()\""
    " style=\"background:#374151;width:auto;padding:8px 16px;font-size:13px;margin:0 0 0 8px\">Listen</button>"
    "</div>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-hardware\">"
    "<form method=\"POST\" action=\"/save\">"
    "<div class=\"card\"><h2>LED</h2>"
    "<label class=\"tip\" data-tip=\"Status LED brightness. 0 = off, 255 = maximum. Values of 20-50 work well indoors. Blue = connecting, solid green = connected, blinking green = streaming, orange = WiFi failed, red = setup mode.\">Brightness &nbsp;<span class=\"val\" id=\"bv\">%d</span></label>"
    "<input type=\"range\" name=\"led_brightness\" min=\"0\" max=\"255\" value=\"%d\""
    " oninput=\"bv.textContent=this.value\">"
    "</div>"
    "<div class=\"card\"><h2>Battery</h2>"
    "<label class=\"tip\" data-tip=\"Battery chemistry, used to fill in sensible Low/Nominal/Full voltage presets below. Pick Custom to enter your own pack voltages directly. Requires an INA219 sensor wired to SDA/SCL - if none is detected, this card is just informational and has no effect.\">Chemistry</label>"
    "<select name=\"batt_chemistry\" id=\"battChem\" onchange=\"applyBattPreset()\">"
    "<option value=\"0\"%s>Li-ion / LiPo</option>"
    "<option value=\"1\"%s>LiFePO4</option>"
    "<option value=\"2\"%s>Custom</option>"
    "</select>"
    "<div id=\"battCellsWrap\"><label class=\"tip\" data-tip=\"Number of cells in series (1S-4S). Multiplies the per-cell chemistry preset to get pack voltage. Ignored when Chemistry is Custom.\">Cell Count</label>"
    "<select name=\"batt_cells\" id=\"battCells\" onchange=\"applyBattPreset()\">"
    "<option value=\"1\"%s>1S</option>"
    "<option value=\"2\"%s>2S</option>"
    "<option value=\"3\"%s>3S</option>"
    "<option value=\"4\"%s>4S</option>"
    "</select></div>"
    "<div class=\"row\" style=\"grid-template-columns:1fr 1fr 1fr\">"
    "<div><label class=\"tip\" data-tip=\"Voltage at or below which the Status card shows a LOW warning.\">Low (V)</label>"
    "<input type=\"number\" step=\"0.01\" name=\"batt_low_v\" id=\"battLow\" value=\"%.2f\"></div>"
    "<div><label class=\"tip\" data-tip=\"Informational reference point, not currently used in any calculation.\">Nominal (V)</label>"
    "<input type=\"number\" step=\"0.01\" name=\"batt_nom_v\" id=\"battNom\" value=\"%.2f\"></div>"
    "<div><label class=\"tip\" data-tip=\"Voltage treated as 100%% for the Status card's battery percentage estimate.\">Full (V)</label>"
    "<input type=\"number\" step=\"0.01\" name=\"batt_full_v\" id=\"battFull\" value=\"%.2f\"></div>"
    "</div>"
    "</div>"
    "<input type=\"hidden\" name=\"tab\" value=\"hardware\">"
    "<button type=\"submit\">Save</button>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 0;text-align:center\">"
    "LED and Battery settings apply instantly.</p>"
    "</form>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-diagnostics\">"
    "<form method=\"POST\" action=\"/save\">"
    "<div class=\"card\"><h2>Diagnostics</h2>"
    "<label class=\"tip\" data-tip=\"Reboots the device if the audio reader task ever stops producing data entirely for about a minute (a wedged driver call, not just a quiet/dead mic - see Mic Health above for that). Off means a stall like that needs a manual power cycle to clear.\">Stall Watchdog</label>"
    "<select name=\"watchdog_enabled\">"
    "<option value=\"1\"%s>Enabled</option>"
    "<option value=\"0\"%s>Disabled</option>"
    "</select>"
    "</div>"
    "<input type=\"hidden\" name=\"tab\" value=\"diagnostics\">"
    "<button type=\"submit\">Save</button>"
    "<p style=\"font-size:11px;color:#6b7280;margin:6px 0 0;text-align:center\">"
    "Applies instantly.</p>"
    "</form>"
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-firmware\">"
    "<div class=\"card\"><h2>Firmware</h2>"
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
    "</div>"
    "<div class=\"tab-panel\" id=\"tab-advanced\">"
    "<div class=\"card\"><h2>Device</h2>"
    "<p style=\"font-size:12px;color:#6b7280;margin:0 0 10px\">Reboots the "
    "device. Settings are kept; any active stream is interrupted.</p>"
    "<form method=\"POST\" action=\"/reboot\" onsubmit=\"return confirm('Reboot the device? "
    "Any active stream will be interrupted.');\">"
    "<button type=\"submit\" style=\"background:#374151\">Reboot</button>"
    "</form>"
    "</div>"
    "<div class=\"card\"><h2>Danger Zone</h2>"
    "<p style=\"font-size:12px;color:#6b7280;margin:0 0 10px\">Erases saved WiFi and audio "
    "settings and reboots into setup mode (broadcasting \"" WIFI_AP_SSID "\").</p>"
    "<form method=\"POST\" action=\"/reset\" onsubmit=\"return confirm('Factory reset? This erases "
    "saved WiFi and audio settings.');\">"
    "<button type=\"submit\" style=\"background:#dc2626\">Factory Reset</button>"
    "</form>"
    "</div>"
    "</div>"
    "</div>"
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
    "document.getElementById('stUpM').textContent=stFmt(j.uptime);"
    "document.getElementById('stWifi').textContent=j.rssi?j.rssi+' dBm':'\\u2013';"
    "document.getElementById('stWifiM').textContent=j.rssi?j.rssi+' dBm':'\\u2013';"
    "document.getElementById('stHeap').textContent=Math.round(j.heap/1024)+' KB';"
    "document.getElementById('stCli').textContent=j.clients+' / '+j.max_clients;"
    "document.getElementById('stStr').textContent=j.streaming?j.streaming+' active':'no';"
    "document.getElementById('stStrM').textContent=j.streaming?j.streaming+' active':'no';"
    "document.getElementById('stOvr').textContent=j.overruns;"
    "document.getElementById('stDma').textContent=j.dma_ovf;"
    "var sm=document.getElementById('stMic'),smM=document.getElementById('stMicM');"
    "if(!j.mic_present){sm.textContent=smM.textContent='NO MIC';sm.style.color=smM.style.color='#f87171';}"
    "else if(j.mic){sm.textContent=smM.textContent='OK';sm.style.color=smM.style.color='';}"
    "else{sm.textContent=smM.textContent='SILENT '+stFmt(j.mic_silent);sm.style.color=smM.style.color='#f87171';}"
    "var sw=document.getElementById('stWd'),swM=document.getElementById('stWdM');"
    "if(!j.wd_enabled){sw.textContent=swM.textContent='OFF';sw.style.color=swM.style.color='';}"
    "else if(!j.wd_stall){sw.textContent=swM.textContent='OK';sw.style.color=swM.style.color='';}"
    "else{sw.textContent=swM.textContent='STALLED '+stFmt(j.wd_stall);sw.style.color=swM.style.color='#f87171';}"
    "var sb=document.getElementById('stBatt'),sbM=document.getElementById('stBattM');"
    "var bi=document.getElementById('battIcon');"
    "if(!j.batt_present){sb.textContent=sbM.textContent='\\u2013';sb.style.color=sbM.style.color='';bi.style.display='none';}"
    "else{var btxt=(j.batt_mv/1000).toFixed(2)+' V ('+j.batt_pct+'%%)'"
    "+(j.batt_low?' LOW':'');sb.textContent=sbM.textContent=btxt;"
    "sb.style.color=sbM.style.color=j.batt_low?'#f87171':'';"
    "bi.style.display='flex';bi.classList.toggle('low',j.batt_low);"
    "var filled=Math.round(j.batt_pct/10),bars=bi.querySelectorAll('.bar');"
    "for(var k=0;k<bars.length;k++)bars[k].classList.toggle('filled',k<filled);}"
    "}).catch(function(){});"
    "}"
    "setInterval(stPoll,2000);stPoll();"
    "var $=function(i){return document.getElementById(i);};"
    // Frequency response of the exact filter the firmware runs: N cascaded
    // 1st-order HPF stages H(z)=a(1-1/z)/(1-a/z) shelf-blended with the dry
    // signal: Ht = k + (1-k)*H^N, plotted in dB on a log frequency axis.
    "window.drawHpf=function(){"
    "var c=$('hg');if(!c)return;var g=c.getContext('2d');"
    // match the backing store to the on-screen CSS size × devicePixelRatio,
    // otherwise the fixed 600px bitmap gets stretched to the card width and
    // smears the axis text
    "var pr=window.devicePixelRatio||1,rc=c.getBoundingClientRect();"
    "var bw=Math.round(rc.width*pr),bh=Math.round(rc.height*pr);"
    "if(bw>0&&(c.width!=bw||c.height!=bh)){c.width=bw;c.height=bh;}"
    "var W=c.width,H=c.height;"
    "var fc=parseInt($('hpfIn').value),N=parseInt($('slopeSel').value),"
    "dp=parseInt($('depthIn').value),fs=parseInt($('srSel').value);"
    "g.fillStyle='#111827';g.fillRect(0,0,W,H);"
    "var fmin=20,fmax=fs/2,dbMin=-66,dbMax=6;"
    "function fx(f){return W*Math.log(f/fmin)/Math.log(fmax/fmin);}"
    "function fy(db){return H*(dbMax-db)/(dbMax-dbMin);}"
    "g.strokeStyle='#1f2937';g.fillStyle='#4b5563';"
    "g.font=(10*pr)+'px monospace';g.lineWidth=pr;"
    "[-60,-40,-20,0].forEach(function(db){var y=fy(db);"
    "g.beginPath();g.moveTo(0,y);g.lineTo(W,y);g.stroke();g.fillText(db+' dB',4*pr,y-3*pr);});"
    "[100,1000,10000].forEach(function(f){if(f<fmax){var px=fx(f);"
    "g.beginPath();g.moveTo(px,0);g.lineTo(px,H);g.stroke();"
    "g.fillText(f>=1000?(f/1000)+'k':''+f,px+3*pr,H-4*pr);}});"
    "if(fc>0){g.strokeStyle='#f59e0b';g.setLineDash([3*pr,3*pr]);"
    "var cx=fx(Math.max(fc,fmin));g.beginPath();g.moveTo(cx,0);g.lineTo(cx,H);g.stroke();g.setLineDash([]);}"
    "var a=fc>0?Math.max(0,1-2*Math.PI*fc/fs):0,k=dp>=60?0:Math.pow(10,-dp/20);"
    "g.strokeStyle='#60a5fa';g.lineWidth=2*pr;g.beginPath();"
    "for(var p=0;p<=W;p+=2){"
    "var f=fmin*Math.pow(fmax/fmin,p/W),db=0;"
    "if(a>0){"
    "var w=2*Math.PI*f/fs,cw=Math.cos(w),sw=Math.sin(w);"
    "var nr=a*(1-cw),ni=a*sw,dr=1-a*cw,di=a*sw;"
    "var dd=dr*dr+di*di,hr=(nr*dr+ni*di)/dd,hi=(ni*dr-nr*di)/dd;"
    "var rr=1,ri=0;"
    "for(var q=0;q<N;q++){var t=rr*hr-ri*hi;ri=rr*hi+ri*hr;rr=t;}"
    "rr=k+(1-k)*rr;ri=(1-k)*ri;"
    "db=20*Math.log(Math.sqrt(rr*rr+ri*ri)+1e-9)/Math.LN10;"
    "}"
    "if(db>dbMax)db=dbMax;if(db<dbMin)db=dbMin;"
    "var py=fy(db);"
    "p===0?g.moveTo(0,py):g.lineTo(p,py);"
    "}"
    "g.stroke();"
    "};"
    "var lsnCtx=null,lsnAbort=null,lsnEl=null;"
    "function lsnStop(){"
    "if(lsnAbort){try{lsnAbort.abort();}catch(e){}lsnAbort=null;}"
    "if(lsnEl){try{lsnEl.pause();lsnEl.srcObject=null;}catch(e){}lsnEl=null;}"
    "if(lsnCtx){try{lsnCtx.close();}catch(e){}lsnCtx=null;}"
    "$('lsnbtn').textContent='Listen';"
    "}"
    "window.toggleListen=function(){"
    "if(lsnAbort){lsnStop();return;}"
    "var b=$('lsnbtn');"
    // iOS only unmutes audio objects created synchronously inside the tap
    // gesture, so the whole output path (context + media element) is built
    // here, before any network I/O. Routing through an <audio> element
    // also bypasses the iPhone ring/silent switch, which mutes bare
    // WebAudio output.
    "lsnCtx=new (window.AudioContext||window.webkitAudioContext)();"
    "if(lsnCtx.resume)lsnCtx.resume();"
    "var dst=lsnCtx.createMediaStreamDestination();"
    "lsnEl=document.createElement('audio');"
    "lsnEl.setAttribute('playsinline','');"
    "lsnEl.srcObject=dst.stream;"
    "lsnEl.play();"
    "lsnAbort=new AbortController();"
    "b.textContent='Connecting\\u2026';"
    "fetch('/listen',{signal:lsnAbort.signal}).then(function(r){"
    "if(!r.ok){return r.text().then(function(t){throw t||('HTTP '+r.status);});}"
    "var sr=parseInt(r.headers.get('X-Sample-Rate'))||48000;"
    "var next=0,carry=new Uint8Array(0),rd=r.body.getReader();"
    "b.textContent='Stop';"
    "(function pump(){"
    "rd.read().then(function(res){"
    "if(res.done||!lsnCtx){lsnStop();return;}"
    "var d;"
    "if(carry.length){d=new Uint8Array(carry.length+res.value.length);"
    "d.set(carry);d.set(res.value,carry.length);}else{d=res.value;}"
    "var n=d.length>>1;"
    "if(n>0){"
    "var v=new DataView(d.buffer,d.byteOffset,n*2),f=new Float32Array(n);"
    "for(var i=0;i<n;i++)f[i]=v.getInt16(i*2,false)/32768;"
    // buffers keep the stream's own sample rate; WebAudio resamples to the
    // context rate on playback, so no {sampleRate} option is needed (iOS
    // Safari historically mishandles it anyway)
    "var ab=lsnCtx.createBuffer(1,n,sr);"
    "if(ab.copyToChannel)ab.copyToChannel(f,0);else ab.getChannelData(0).set(f);"
    "var s=lsnCtx.createBufferSource();s.buffer=ab;s.connect(dst);"
    "if(next<lsnCtx.currentTime+0.05)next=lsnCtx.currentTime+0.3;"
    "s.start(next);next+=n/sr;"
    "}"
    "carry=(d.length&1)?d.slice(n*2):new Uint8Array(0);"
    "pump();"
    "}).catch(function(){lsnStop();});"
    "})();"
    "}).catch(function(e){lsnStop();if(e&&e.name!=='AbortError')alert('Preview failed: '+e);});"
    "};"
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
    "document.getElementById('mmSel').disabled=usb;"
    "document.getElementById('mmWrap').classList.toggle('dim',usb);"
    "};"
    "toggleGainShift();"
    // Per-cell [low,nominal,full] volts for each preset chemistry — pack
    // voltage is this times the selected cell count. Custom skips this
    // table entirely and leaves the three fields as whatever's saved/typed.
    "var battTable=[[3.30,3.70,4.20],[2.80,3.20,3.65]];"
    "function battUI(){"
    "var custom=$('battChem').value=='2';"
    "$('battCells').disabled=custom;"
    "$('battCellsWrap').classList.toggle('dim',custom);"
    "}"
    "window.applyBattPreset=function(){"
    "battUI();"
    "var chem=+$('battChem').value;"
    "if(chem==2)return;"
    "var t=battTable[chem],cells=+$('battCells').value;"
    "$('battLow').value=(t[0]*cells).toFixed(2);"
    "$('battNom').value=(t[1]*cells).toFixed(2);"
    "$('battFull').value=(t[2]*cells).toFixed(2);"
    "};"
    "battUI();"
    "var dv=$('depthIn').value;"
    "$('hdv').textContent=dv>=60?'Full':dv+' dB';"
    "drawHpf();"
    "window.addEventListener('resize',drawHpf);"
    "function showTab(id){"
    "document.querySelectorAll('.tab-panel').forEach(function(p){p.classList.toggle('active',p.id==='tab-'+id);});"
    "document.querySelectorAll('.tabbtn').forEach(function(b){b.classList.toggle('active',b.dataset.tab===id);});"
    "location.hash=id;"
    "if(id==='audio')drawHpf();"
    "}"
    "document.querySelectorAll('.tabbtn').forEach(function(b){b.onclick=function(){showTab(b.dataset.tab);};});"
    "showTab(location.hash.slice(1)||'dashboard');"
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

// True if `key=` appears anywhere in the POST body, whether or not it has a
// value. Distinguishes "field absent from this form" (leave untouched) from
// "field present but blank" (e.g. password intentionally cleared) — a
// distinction get_field()'s empty-string return can't make on its own.
static bool has_field(const char *body, const char *key)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "%s=", key);
    return strstr(body, needle) != NULL;
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

    const size_t bufsz = 32768;
    char *buf = malloc(bufsz);
    if (!buf) return ESP_ERR_NO_MEM;

    int len = snprintf(buf, bufsz, s_html,
        status_line,
        name_esc,
        ssid_esc,
        pass_esc,
        (int)g_config.wifi_tx_power_dbm, (int)g_config.wifi_tx_power_dbm,
        g_config.audio_source == AUDIO_SOURCE_I2S ? " selected" : "",
        g_config.audio_source == AUDIO_SOURCE_USB ? " selected" : "",
        g_config.mic_model == MIC_MODEL_INMP441 ? " selected" : "",
        g_config.mic_model == MIC_MODEL_SPH0645 ? " selected" : "",
        g_config.sample_rate ==  8000 ? " selected" : "",
        g_config.sample_rate == 16000 ? " selected" : "",
        g_config.sample_rate == 22050 ? " selected" : "",
        g_config.sample_rate == 32000 ? " selected" : "",
        g_config.sample_rate == 44100 ? " selected" : "",
        g_config.sample_rate == 48000 ? " selected" : "",
        (int)g_config.gain_shift,  (int)g_config.gain_shift,
        (int)g_config.gain_mult,   (int)g_config.gain_mult,
        (int)g_config.hpf_freq, (int)g_config.hpf_freq,
        g_config.hpf_slope == 1 ? " selected" : "",
        g_config.hpf_slope == 2 ? " selected" : "",
        g_config.hpf_slope == 3 ? " selected" : "",
        g_config.hpf_slope == 4 ? " selected" : "",
        (int)g_config.hpf_depth, (int)g_config.hpf_depth,
        (int)g_config.led_brightness, (int)g_config.led_brightness,
        g_config.batt_chemistry == 0 ? " selected" : "",
        g_config.batt_chemistry == 1 ? " selected" : "",
        g_config.batt_chemistry == 2 ? " selected" : "",
        g_config.batt_cells == 1 ? " selected" : "",
        g_config.batt_cells == 2 ? " selected" : "",
        g_config.batt_cells == 3 ? " selected" : "",
        g_config.batt_cells == 4 ? " selected" : "",
        g_config.batt_low_mv  / 1000.0,
        g_config.batt_nom_mv  / 1000.0,
        g_config.batt_full_mv / 1000.0,
        g_config.watchdog_enabled ? " selected" : "",
        !g_config.watchdog_enabled ? " selected" : "",
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
    if (body_len <= 0 || body_len > 1023) {
        // Never parse a truncated form: a field cut mid-value (e.g. a
        // clipped password) would still pass validation and get persisted.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form size");
        return ESP_FAIL;
    }

    char *body = malloc(body_len + 1);
    if (!body) return ESP_ERR_NO_MEM;

    // recv can deliver the body in pieces — loop until it's all here, for
    // the same reason as above.
    int received = 0, timeouts = 0;
    while (received < body_len) {
        int n = httpd_req_recv(req, body + received, body_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload interrupted");
            return ESP_FAIL;
        }
        timeouts = 0;
        received += n;
    }
    body[received] = '\0';

    // Snapshot the fields that require a reboot to take effect (mDNS/WiFi
    // re-init, mic driver + I2S clock re-init). Everything else — gains,
    // HPF, LED brightness — is read from g_config live by the audio/LED
    // code and applies the moment the struct is updated.
    char     old_name[sizeof(g_config.device_name)];
    char     old_ssid[sizeof(g_config.wifi_ssid)];
    char     old_pass[sizeof(g_config.wifi_password)];
    uint8_t  old_source = g_config.audio_source;
    uint8_t  old_model  = g_config.mic_model;
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

    // Unlike other fields, blank is a meaningful value here (clears a saved
    // password for an open network) rather than "leave unchanged" — but
    // only when the field is actually part of this form (Device & Network
    // tab). Other tabs' forms don't include a password input at all.
    get_field(body, "password", val, sizeof(val));
    if (has_field(body, "password"))
        strlcpy(g_config.wifi_password, val, sizeof(g_config.wifi_password));

    get_field(body, "tx_power", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= WIFI_TX_POWER_DBM_MIN && v <= WIFI_TX_POWER_DBM_MAX)
            g_config.wifi_tx_power_dbm = (uint8_t)v;
    }

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

    get_field(body, "hpf_slope", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 1 && v <= 4) g_config.hpf_slope = (uint8_t)v;
    }

    get_field(body, "hpf_depth", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 0 && v <= 60) g_config.hpf_depth = (uint8_t)v;
    }

    get_field(body, "mic_model", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v == MIC_MODEL_INMP441 || v == MIC_MODEL_SPH0645)
            g_config.mic_model = (uint8_t)v;
    }

    get_field(body, "batt_chemistry", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 0 && v <= 2) g_config.batt_chemistry = (uint8_t)v;
    }

    get_field(body, "batt_cells", val, sizeof(val));
    if (val[0]) {
        int v = atoi(val);
        if (v >= 1 && v <= 4) g_config.batt_cells = (uint8_t)v;
    }

    // Voltage fields arrive as volts (e.g. "3.30") from the number inputs —
    // stored internally as mV, same unit battery_monitor_voltage_mv() reads.
    get_field(body, "batt_low_v", val, sizeof(val));
    if (val[0]) {
        double v = atof(val);
        if (v >= 0 && v <= 60) g_config.batt_low_mv = (uint16_t)(v * 1000 + 0.5);
    }

    get_field(body, "batt_nom_v", val, sizeof(val));
    if (val[0]) {
        double v = atof(val);
        if (v >= 0 && v <= 60) g_config.batt_nom_mv = (uint16_t)(v * 1000 + 0.5);
    }

    get_field(body, "batt_full_v", val, sizeof(val));
    if (val[0]) {
        double v = atof(val);
        if (v >= 0 && v <= 60) g_config.batt_full_mv = (uint16_t)(v * 1000 + 0.5);
    }

    get_field(body, "watchdog_enabled", val, sizeof(val));
    if (val[0]) g_config.watchdog_enabled = atoi(val) ? 1 : 0;

    // Which tab's form posted this — used only to send the browser back to
    // that tab after the reload, so saving e.g. Audio settings doesn't dump
    // the user back at the top of the page. Whitelisted before reflecting
    // into the response HTML below.
    char tab[24];
    get_field(body, "tab", tab, sizeof(tab));
    static const char *valid_tabs[] = {
        "dashboard", "device", "audio", "hardware", "diagnostics", "firmware", "advanced"
    };
    const char *redirect = "/";
    for (size_t i = 0; i < sizeof(valid_tabs) / sizeof(valid_tabs[0]); i++) {
        if (strcmp(tab, valid_tabs[i]) == 0) {
            redirect = valid_tabs[i];
            break;
        }
    }
    char redirect_url[32];
    if (redirect[0] != '/')
        snprintf(redirect_url, sizeof(redirect_url), "/#%s", redirect);
    else
        strlcpy(redirect_url, redirect, sizeof(redirect_url));

    free(body);
    app_config_save();
    wifi_manager_apply_tx_power();

    bool reboot_needed =
        strcmp(old_name, g_config.device_name)   != 0 ||
        strcmp(old_ssid, g_config.wifi_ssid)     != 0 ||
        strcmp(old_pass, g_config.wifi_password) != 0 ||
        old_source != g_config.audio_source ||
        old_model  != g_config.mic_model    ||
        old_rate   != g_config.sample_rate;

    httpd_resp_set_type(req, "text/html");

    char resp[512];
    int rlen;
    if (reboot_needed) {
        rlen = snprintf(resp, sizeof(resp),
            "<!DOCTYPE html><html><head>"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Warbler32</title></head>"
            "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
            "text-align:center;padding:60px 16px\">"
            "<h2 style=\"color:#34d399\">Saved!</h2>"
            "<p>Device is rebooting&hellip;</p>"
            "<p style=\"color:#6b7280;font-size:13px\">Page will reload in 8 seconds.</p>"
            "<script>setTimeout(()=>{location.href='%s'},8000)</script>"
            "</body></html>", redirect_url);
        httpd_resp_send(req, resp, rlen);
        xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    } else {
        ESP_LOGI(TAG, "config saved, applied live (no reboot)");
        rlen = snprintf(resp, sizeof(resp),
            "<!DOCTYPE html><html><head>"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Warbler32</title></head>"
            "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
            "text-align:center;padding:60px 16px\">"
            "<h2 style=\"color:#34d399\">Saved!</h2>"
            "<p>Settings applied &mdash; no reboot needed.</p>"
            "<script>setTimeout(()=>{location.href='%s'},1200)</script>"
            "</body></html>", redirect_url);
        httpd_resp_send(req, resp, rlen);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /reboot — plain reboot, settings untouched
// ---------------------------------------------------------------------------
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    static const char resp[] =
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Warbler32</title></head>"
        "<body style=\"font-family:system-ui;background:#111827;color:#e5e7eb;"
        "text-align:center;padding:60px 16px\">"
        "<h2 style=\"color:#34d399\">Rebooting&hellip;</h2>"
        "<p>Settings are unchanged. The device will be back shortly.</p>"
        "<p style=\"color:#6b7280;font-size:13px\">Page will reload in 8 seconds.</p>"
        "<script>setTimeout(()=>{location.href='/'},8000)</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
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
    // snprintf returns the would-be length — never send past the buffer
    if (len >= (int)sizeof(json)) len = sizeof(json) - 1;
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
// GET /listen — live PCM preview stream (raw big-endian L16, chunked)
//
// The stream runs until the browser disconnects, so it must not occupy the
// single httpd task: the handler detaches the request with
// httpd_req_async_handler_begin() and hands it to a small worker task.
// ---------------------------------------------------------------------------
static atomic_bool s_preview_busy = false;

static void listen_stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    // ~20 ms of audio per chunk at 48 kHz; smaller rates just chunk shorter
    char buf[1920];

    int reader = audio_pipeline_subscribe(false);
    if (reader < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No audio stream slot free");
        goto out;
    }

    // Detect a vanished browser quickly: without this, a dead connection
    // blocks the chunk send for the full default socket timeout.
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    char sr[16];
    snprintf(sr, sizeof(sr), "%lu", (unsigned long)g_config.sample_rate);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Sample-Rate", sr);

    ESP_LOGI(TAG, "preview stream started");
    for (;;) {
        size_t got = audio_pipeline_read(reader, (uint8_t *)buf, sizeof(buf), 200);
        if (got == 0) continue;
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) break;  // client gone
    }
    ESP_LOGI(TAG, "preview stream ended");

    audio_pipeline_unsubscribe(reader);
    httpd_resp_send_chunk(req, NULL, 0);
out:
    httpd_req_async_handler_complete(req);
    atomic_store(&s_preview_busy, false);
    vTaskDelete(NULL);
}

static esp_err_t listen_get_handler(httpd_req_t *req)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_preview_busy, &expected, true)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Preview already in use");
        return ESP_FAIL;
    }

    httpd_req_t *async_req;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        atomic_store(&s_preview_busy, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async setup failed");
        return ESP_FAIL;
    }
    if (xTaskCreate(listen_stream_task, "preview", 4096, async_req, 3, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        atomic_store(&s_preview_busy, false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
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

    // Battery percent is a simple linear estimate between the configured
    // Low/Full voltages — good enough for a status display, not lab-grade.
    bool     batt_present = battery_monitor_present();
    uint16_t batt_mv      = battery_monitor_voltage_mv();
    int      batt_pct     = 0;
    if (batt_present && g_config.batt_full_mv > g_config.batt_low_mv) {
        int span = (int)g_config.batt_full_mv - (int)g_config.batt_low_mv;
        int rel  = (int)batt_mv - (int)g_config.batt_low_mv;
        batt_pct = (rel * 100) / span;
        if (batt_pct < 0)   batt_pct = 0;
        if (batt_pct > 100) batt_pct = 100;
    }
    bool batt_low = batt_present && batt_mv < g_config.batt_low_mv;

    char json[480];
    int len = snprintf(json, sizeof(json),
        "{\"uptime\":%lld,\"heap\":%u,\"heap_min\":%u,\"psram\":%u,"
        "\"rssi\":%d,\"clients\":%d,\"max_clients\":%d,\"streaming\":%d,"
        "\"overruns\":%u,\"dma_ovf\":%u,\"peak\":%d,\"preview\":%d,"
        "\"mic\":%d,\"mic_silent\":%u,\"mic_present\":%d,"
        "\"wd_enabled\":%d,\"wd_stall\":%u,"
        "\"batt_present\":%d,\"batt_mv\":%u,\"batt_pct\":%d,\"batt_low\":%d,"
        "\"version\":\"%s\",\"variant\":\"%s\"}",
        esp_timer_get_time() / 1000000,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        rssi,
        rtsp_server_client_count(), RTSP_MAX_CLIENTS,
        rtsp_server_streaming_count(),
        (unsigned)audio_pipeline_get_overruns(),
        (unsigned)audio_pipeline_get_dma_overflows(),
        audio_pipeline_get_peak_pct(),
        atomic_load(&s_preview_busy) ? 1 : 0,
        mic_health_ok() ? 1 : 0, (unsigned)mic_health_silent_secs(),
        audio_pipeline_is_active() ? 1 : 0,
        g_config.watchdog_enabled ? 1 : 0, (unsigned)pipeline_watchdog_stall_secs(),
        batt_present ? 1 : 0, (unsigned)batt_mv, batt_pct, batt_low ? 1 : 0,
        esp_app_get_description()->version, ota_board_variant());

    // snprintf returns the would-be length — never send past the buffer
    if (len >= (int)sizeof(json)) len = sizeof(json) - 1;

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
    cfg.max_uri_handlers  = 16;
    cfg.stack_size        = 12288;  // TLS handshake runs in-handler for /update/check
    // Default (7) + 3 reserved-for-httpd-internals == the entire global
    // CONFIG_LWIP_MAX_SOCKETS budget, leaving nothing for the RTSP
    // listener/clients, mDNS, or the OTA HTTPS client — under enough
    // concurrent web traffic the whole device runs out of sockets and
    // every other subsystem starts failing ("Could not reach GitHub",
    // httpd itself refusing new connections). Capped here, with the
    // budget raised in sdkconfig.defaults to fit everything comfortably.
    cfg.max_open_sockets  = 4;

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
    static const httpd_uri_t post_reboot = {
        .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post_handler,
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
    static const httpd_uri_t get_listen = {
        .uri = "/listen", .method = HTTP_GET, .handler = listen_get_handler,
    };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_save);
    httpd_register_uri_handler(server, &get_level);
    httpd_register_uri_handler(server, &post_reboot);
    httpd_register_uri_handler(server, &post_reset);
    httpd_register_uri_handler(server, &post_ota);
    httpd_register_uri_handler(server, &get_status);
    httpd_register_uri_handler(server, &post_upd_check);
    httpd_register_uri_handler(server, &post_upd_install);
    httpd_register_uri_handler(server, &get_upd_progress);
    httpd_register_uri_handler(server, &get_listen);

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
