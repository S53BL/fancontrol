// webserver.cpp — Web vmesnik za fancontrol
// Single-page app: Dashboard / Ventilator / Kalibracija / Sistem
// WiFi statični IP, NTP, mDNS, OTA flash

#include "webserver.h"
#include "globals.h"
#include "config.h"
#include "logging.h"
#include "graph_store.h"
#include "fan.h"
#include "monitor.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ezTime.h>
#include <HTTPClient.h>
#include "wifi_config.h"
#include "uplot_assets.h"

static AsyncWebServer server(80);

// =====================================================================
// fetchWeather — Open-Meteo API (brez API ključa), klic iz main loop
// =====================================================================
void fetchWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        weatherData.err = 1;
        LOG_WARN("WX", "fetchWeather() — WiFi ni povezan");
        return;
    }

    char url[300];
    snprintf(url, sizeof(url), WEATHER_URL_TEMPLATE, WEATHER_LAT, WEATHER_LON);
    LOG_INFO("WX", "URL: %s", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000);
    int code = http.GET();

    if (code != 200) {
        weatherData.err = 1;
        LOG_WARN("WX", "HTTP napaka: %d", code);
        http.end();
        return;
    }

    // getString() — deluje tudi pri chunked transfer (Content-Length: -1)
    String wxBody = http.getString();
    http.end();

    int bodyLen = wxBody.length();
    LOG_INFO("WX", "HTTP body: %d bytes", bodyLen);

    if (bodyLen < 50) {
        weatherData.err = 2;
        LOG_WARN("WX", "HTTP body prekratek: %d bytes", bodyLen);
        return;
    }

    // PSRAM buffer — kopiraj String v char* za deserializeJson
    static char* _wxBuf = nullptr;
    if (!_wxBuf) _wxBuf = (char*)ps_malloc(WEATHER_BUFFER_SIZE);
    if (!_wxBuf) {
        weatherData.err = 1;
        LOG_WARN("WX", "ps_malloc za weather buffer failed");
        return;
    }
    memset(_wxBuf, 0, WEATHER_BUFFER_SIZE);
    strncpy(_wxBuf, wxBody.c_str(), WEATHER_BUFFER_SIZE - 1);
    wxBody = String(); // sprosti heap String takoj

    // Filter: current + daily[0] (sunrise, sunset)
    StaticJsonDocument<128> filter;
    filter["current"]["temperature_2m"]       = true;
    filter["current"]["relative_humidity_2m"] = true;
    filter["current"]["weather_code"]          = true;
    filter["daily"]["sunrise"]                 = true;
    filter["daily"]["sunset"]                  = true;

    // DynamicJsonDocument — daily array potrebuje več prostora
    DynamicJsonDocument doc(1024);
    DeserializationError jerr = deserializeJson(doc, _wxBuf,
                                                DeserializationOption::Filter(filter));
    if (jerr) {
        weatherData.err = 2;
        LOG_WARN("WX", "JSON parse napaka: %s", jerr.c_str());
        LOG_WARN("WX", "Prvih 200 znakov odgovora: %.200s", _wxBuf);
        return;
    }

    portENTER_CRITICAL(&dataMux);

    weatherData.outTemp = doc["current"]["temperature_2m"]        | 0.0f;
    weatherData.outHum  = doc["current"]["relative_humidity_2m"]  | 0;
    weatherData.wxCode  = doc["current"]["weather_code"]          | 0;

    // Sunrise / sunset — format iz API: "2026-06-01T05:23" → vzamemo samo HH:MM
    const char* srRaw = doc["daily"]["sunrise"][0] | nullptr;
    const char* ssRaw = doc["daily"]["sunset"][0]  | nullptr;

    if (srRaw && strlen(srRaw) >= 16) {
        // Format: "2026-06-01T05:23" — HH:MM je na poziciji 11
        strncpy(weatherData.sunrise, srRaw + 11, 5);
        weatherData.sunrise[5] = '\0';
    } else {
        strncpy(weatherData.sunrise, "--:--", 6);
    }

    if (ssRaw && strlen(ssRaw) >= 16) {
        strncpy(weatherData.sunset, ssRaw + 11, 5);
        weatherData.sunset[5] = '\0';
    } else {
        strncpy(weatherData.sunset, "--:--", 6);
    }

    // isNight — izračun iz sunrise/sunset (HH:MM formata)
    weatherData.isNight = false;
    if (timeSynced &&
        weatherData.sunrise[0] != '-' &&
        weatherData.sunset[0]  != '-') {
        int curH  = myTZ.hour();
        int curM  = myTZ.minute();
        int curMin = curH * 60 + curM;

        int srH = atoi(weatherData.sunrise);
        int srM = atoi(weatherData.sunrise + 3);
        int ssH = atoi(weatherData.sunset);
        int ssM = atoi(weatherData.sunset + 3);

        int srMin = srH * 60 + srM;
        int ssMin = ssH * 60 + ssM;

        weatherData.isNight = (curMin < srMin || curMin > ssMin);
    } else if (timeSynced) {
        // Fallback — stara logika če sunrise/sunset ni na voljo
        int h = myTZ.hour();
        weatherData.isNight = (h >= 22 || h < 6);
    }

    weatherData.valid = true;
    weatherData.err   = 0;

    portEXIT_CRITICAL(&dataMux);

    LOG_INFO("WX", "Vreme OK: %.1f degC  %d%%  wxCode=%d  vzh=%s  zah=%s  noc=%s",
             weatherData.outTemp,
             (int)weatherData.outHum,
             (int)weatherData.wxCode,
             weatherData.sunrise,
             weatherData.sunset,
             weatherData.isNight ? "DA" : "NE");
}

// =====================================================================
// WiFi — statični IP
// =====================================================================
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    IPAddress ip, gw, sn, dns;
    ip.fromString(WIFI_STATIC_IP);
    gw.fromString(WIFI_STATIC_GW);
    sn.fromString(WIFI_STATIC_SUBNET);
    dns.fromString(WIFI_STATIC_DNS);
    WiFi.config(ip, gw, sn, dns);

    // Najprej NVS (če je shranjeno)
    if (strlen(settings.ssid) > 0) {
        LOG_INFO("WIFI", "Connecting (NVS): %s", settings.ssid);
        WiFi.begin(settings.ssid, settings.password);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
        if (WiFi.status() != WL_CONNECTED) {
            LOG_WARN("WIFI", "NVS omrezje ni dosegljivo — poskusam seznam");
            WiFi.disconnect();
        }
    }

    // Fallback — poskusi vsako omrežje iz wifi_config.h po vrsti
    if (WiFi.status() != WL_CONNECTED) {
        for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
            LOG_INFO("WIFI", "Connecting [%d/%d]: %s", i + 1, WIFI_NETWORK_COUNT, WIFI_SSID_LIST[i]);
            WiFi.begin(WIFI_SSID_LIST[i], WIFI_PASS_LIST[i]);
            unsigned long t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(300);
            if (WiFi.status() == WL_CONNECTED) break;
            WiFi.disconnect();
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("WIFI", "Connected. IP: %s RSSI: %d dBm",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        sensorData.err &= ~ERR_WIFI;
    } else {
        LOG_WARN("WIFI", "Connect failed — brez omrezja");
        sensorData.err |= ERR_WIFI;
    }
}

// =====================================================================
// NTP sinhronizacija
// =====================================================================
static void syncNTP() {
    if (WiFi.status() != WL_CONNECTED) return;

    myTZ.setLocation(F("Europe/Ljubljana"));

    for (int i = 0; i < NTP_SERVER_COUNT; i++) {
        LOG_INFO("NTP", "Poskus [%d/%d]: %s", i + 1, NTP_SERVER_COUNT, NTP_SERVER_LIST[i]);
        setServer(NTP_SERVER_LIST[i]);
        waitForSync(8);
        if (timeStatus() != timeNotSet) {
            timeSynced = true;
            lastNtpSyncMs = millis();
            LOG_INFO("NTP", "Synced [%s]: %s",
                     NTP_SERVER_LIST[i],
                     myTZ.dateTime("d.m.Y H:i:s").c_str());
            return;  // uspelo — ne nadaljuj z naslednjimi
        }
        LOG_WARN("NTP", "Timeout [%s] — naslednji...", NTP_SERVER_LIST[i]);
    }

    // Vsi strežniki neuspešni
    LOG_WARN("NTP", "Sync neuspesen — vsi strežniki brez odgovora");
    sensorData.err |= ERR_NTP;
}

// =====================================================================
// OTA HTML — identično vent_SEW, prilagojeno za fancontrol
// =====================================================================
static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="sl"><head><meta charset="UTF-8">
<title>fancontrol OTA Update</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#0d0d0d;color:#e0e0e0;display:flex;flex-direction:column;align-items:center;padding:40px 20px}
h1{color:#00d4ff;margin-bottom:8px}
.sub{color:#888;font-size:14px;margin-bottom:30px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;padding:30px 36px;width:100%;max-width:480px;text-align:center}
input[type=file]{display:block;width:100%;padding:10px;margin:16px 0 20px;background:#2a2a2a;border:2px dashed #555;border-radius:6px;color:#e0e0e0;cursor:pointer}
input[type=file]:hover{border-color:#00d4ff}
.btn{display:inline-block;padding:12px 32px;background:#00d4ff;color:#0d0d0d;border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer;width:100%}
.btn:hover{background:#33dfff}
.btn:disabled{background:#555;color:#888;cursor:not-allowed}
#progress{width:100%;background:#2a2a2a;border-radius:4px;height:18px;margin-top:18px;display:none;overflow:hidden}
#bar{height:100%;background:#00d4ff;width:0;transition:width 0.3s;border-radius:4px}
#status{margin-top:14px;font-size:14px;color:#00d4ff;min-height:20px}
.nav{margin-top:28px;font-size:14px}.nav a{color:#00d4ff;text-decoration:none}
</style></head><body>
<h1>&#11014; OTA Firmware Update</h1>
<p class="sub">fancontrol — ESP32-S3</p>
<div class="card">
<form id="upForm">
<input type="file" id="file" accept=".bin" required>
<button class="btn" id="btn" type="submit">Nalozi firmware</button>
</form>
<div id="progress"><div id="bar"></div></div>
<div id="status"></div>
</div>
<div class="nav"><a href="/">&#8592; Nazaj</a></div>
<script>
document.getElementById('upForm').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('file').files[0];if(!f)return;
  const btn=document.getElementById('btn'),bar=document.getElementById('bar'),
        prog=document.getElementById('progress'),status=document.getElementById('status');
  btn.disabled=true;prog.style.display='block';status.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';status.textContent='Nalaganje: '+p+'%';}};
  xhr.onload=function(){if(xhr.status===200){bar.style.width='100%';bar.style.background='#30d158';status.textContent='Uspelo! Naprava se resetira v 5s...';setTimeout(()=>{location.href='/';},5500);}else{bar.style.background='#ff3b30';status.textContent='Napaka: '+xhr.responseText;btn.disabled=false;}};
  xhr.onerror=function(){status.textContent='Napaka pri prenosu!';btn.disabled=false;};
  const form=new FormData();form.append('update',f);
  xhr.open('POST','/update');xhr.send(form);
};
</script></body></html>
)rawliteral";

// =====================================================================
// Glavna SPA stran — dark tehno tema, 4 tabi
// =====================================================================
static const char MAIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="sl"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FANCONTROL</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d0d0d;color:#e0e0e0;font-family:monospace,system-ui;min-height:100vh}
header{background:#1a1a1a;border-bottom:2px solid #00d4ff;padding:10px 20px;display:flex;justify-content:space-between;align-items:center}
h1{color:#00d4ff;font-size:18px;letter-spacing:4px}
.hr{font-size:11px;color:#888;text-align:right;line-height:1.7}
.hr span{color:#00d4ff}
nav{background:#1a1a1a;border-bottom:1px solid #2a2a2a;display:flex}
.t{padding:11px 18px;cursor:pointer;font-size:12px;color:#555;border-bottom:2px solid transparent;letter-spacing:1px;transition:color .2s}
.t.on{color:#00d4ff;border-bottom-color:#00d4ff}
main{max-width:920px;margin:0 auto;padding:16px}
.pane{display:none}.pane.on{display:block}
.cards{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:14px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:12px 16px;flex:1;min-width:130px}
.ctit{font-size:10px;color:#555;text-transform:uppercase;letter-spacing:1px;margin-bottom:3px}
.cval{font-size:28px;color:#00d4ff;font-weight:bold}
.cunit{font-size:12px;color:#555}
.cpeak{font-size:10px;color:#ff9500;margin-top:3px}
.cdnd{font-size:10px;color:#ff9500;margin-top:3px}
.cw{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:12px;margin-bottom:10px}
.ct2{font-size:10px;color:#555;margin-bottom:6px;letter-spacing:1px;text-transform:uppercase}
.sec{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:16px;margin-bottom:12px}
.sec h3{color:#00d4ff;font-size:13px;letter-spacing:2px;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #2a2a2a;text-transform:uppercase}
.fr{display:flex;align-items:center;gap:10px;margin-bottom:8px}
.fr label{font-size:12px;color:#aaa;min-width:170px}
input[type=text],input[type=number],input[type=password]{background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:5px 9px;border-radius:4px;font-family:monospace;font-size:12px;width:120px}
input[type=checkbox]{width:16px;height:16px;accent-color:#00d4ff}
input:focus{outline:none;border-color:#00d4ff}
.btn{padding:9px 22px;background:#00d4ff;color:#0d0d0d;border:none;border-radius:6px;cursor:pointer;font-weight:bold;font-family:monospace;font-size:12px;letter-spacing:1px}
.btn:hover{background:#33dfff}
.bto{background:#ff9500;color:#0d0d0d}.bto:hover{background:#ffb733}
.bsm{padding:5px 12px;font-size:11px}
.msg{margin-left:10px;font-size:11px}
table{width:100%;border-collapse:collapse;font-size:11px}
th{color:#555;text-align:left;padding:5px 7px;border-bottom:1px solid #2a2a2a;font-size:10px;letter-spacing:1px;text-transform:uppercase}
td{padding:4px 7px;border-bottom:1px solid #161616}
.li{color:#888}.lw{color:#ff9500}.le{color:#ff3b30}
.ir{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #1e1e1e;font-size:12px}
.ik{color:#888}.iv{color:#00d4ff;font-weight:bold}
.eb{display:inline-block;padding:2px 9px;border-radius:4px;font-size:11px}
.eok{background:#0a2a0a;color:#30d158}.efail{background:#2a0a0a;color:#ff3b30}
.ctbl td,th{min-width:60px}
</style></head><body>
<header>
<div><h1>&#9650; FANCONTROL</h1></div>
<div class="hr">
<div>IP: <span id="hip">...</span></div>
<div>Uptime: <span id="hup">...</span></div>
<div><span id="htim">--:--:--</span></div>
</div>
</header>
<nav>
<div class="t on" onclick="sw(0)">DASHBOARD</div>
<div class="t" onclick="sw(1)">VENTILATOR</div>
<div class="t" onclick="sw(2)">KALIBRACIJA</div>
<div class="t" onclick="sw(3)">SISTEM</div>
</nav>
<main>
<!-- TAB 0: DASHBOARD -->
<div class="pane on" id="p0">
<div class="cards">
<div class="card"><div class="ctit">Temperatura</div><div><span class="cval" id="ct">--</span><span class="cunit"> °C</span></div><div class="cpeak" id="pkt"></div></div>
<div class="card"><div class="ctit">Vlažnost</div><div><span class="cval" id="ch">--</span><span class="cunit"> %</span></div></div>
<div class="card"><div class="ctit">Napetost</div><div><span class="cval" id="cv">--</span><span class="cunit"> V</span></div></div>
<div class="card"><div class="ctit">Poraba</div><div><span class="cval" id="cw">--</span><span class="cunit"> W</span></div><div class="cpeak" id="pkw"></div></div>
<div class="card"><div class="ctit">Ventilator</div><div><span class="cval" id="cf">--</span><span class="cunit"> %</span></div><div class="cdnd" id="cdnd"></div><div class="cdnd" id="cman" style="color:#ff6b00"></div></div>
<div class="card"><div class="ctit">&#9889; Napajanje</div><div class="cval" id="mpwr" style="font-size:20px">--</div></div>
<div class="card"><div class="ctit">&#127760; Servisi</div><div><span class="cval" id="msvc" style="font-size:20px">--</span></div><div class="cpeak" id="msvd"></div></div>
</div>
<div id="monBox" class="cw" style="display:none">
<div class="ct2">Mini PC Servisi</div>
<table id="monTbl"><tr><th>Port</th><th>Servis</th><th>Status</th></tr></table>
</div>
<div class="cw">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
    <div class="ct2" style="margin:0">GRAF ZGODOVINE</div>
    <button class="btn bsm" id="zoomReset" onclick="resetZoom()" style="display:none">Reset zoom</button>
  </div>
  <div style="display:flex;flex-wrap:wrap;gap:12px;margin-bottom:8px">
    <label style="font-size:11px;color:#aaa;display:flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" id="cbTemp" checked style="accent-color:#e05252;width:13px;height:13px"> <span style="color:#e05252">Temperatura °C</span></label>
    <label style="font-size:11px;color:#aaa;display:flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" id="cbHum" checked style="accent-color:#5b9bd5;width:13px;height:13px"> <span style="color:#5b9bd5">Vlažnost %</span></label>
    <label style="font-size:11px;color:#aaa;display:flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" id="cbFan" checked style="accent-color:#4ec9b0;width:13px;height:13px"> <span style="color:#4ec9b0">Ventilator %</span></label>
    <label style="font-size:11px;color:#aaa;display:flex;align-items:center;gap:5px;cursor:pointer">
      <input type="checkbox" id="cbWatt" checked style="accent-color:#d4a76a;width:13px;height:13px"> <span style="color:#d4a76a">Poraba W</span></label>
  </div>
  <div id="uplotWrap" style="width:100%"></div>
</div>
</div>
<!-- TAB 1: VENTILATOR -->
<div class="pane" id="p1">
<div class="sec"><h3>Temperaturna krivulja</h3>
<table class="ctbl" style="margin-bottom:14px"><tr><th>Točka</th><th>Temp [°C]</th><th>Fan [%]</th></tr>
<tr><td>1</td><td><input type="number" id="ct0" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp0" min="0" max="100" style="width:70px"></td></tr>
<tr><td>2</td><td><input type="number" id="ct1" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp1" min="0" max="100" style="width:70px"></td></tr>
<tr><td>3</td><td><input type="number" id="ct2" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp2" min="0" max="100" style="width:70px"></td></tr>
<tr><td>4</td><td><input type="number" id="ct3" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp3" min="0" max="100" style="width:70px"></td></tr>
</table></div>
<div class="sec"><h3>Limiti in DND</h3>
<div class="fr"><label>Min hitrost [%]</label><input type="number" id="fMin" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>Max podnevi [%]</label><input type="number" id="fMaxD" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>Max DND [%]</label><input type="number" id="fDndM" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>DND omogočen</label><input type="checkbox" id="dndE"></div>
<div class="fr"><label>DND od ure (0–23)</label><input type="number" id="dndF" min="0" max="23" style="width:70px"></div>
<div class="fr"><label>DND do ure (0–23)</label><input type="number" id="dndT" min="0" max="23" style="width:70px"></div>
<button class="btn" onclick="saveFan()">Shrani</button><span class="msg" id="msgF"></span>
</div>
<div class="sec"><h3>Ročno upravljanje</h3>
<div class="fr" style="margin-bottom:14px">
  <label style="min-width:170px">Način</label>
  <div style="display:flex;align-items:center;gap:10px">
    <span id="modeLabel" style="font-size:12px;color:#aaa;min-width:80px">AVTOMATSKO</span>
    <label style="position:relative;display:inline-block;width:44px;height:24px">
      <input type="checkbox" id="manToggle" style="opacity:0;width:0;height:0" onchange="onManToggle()">
      <span id="manSliderToggle" style="position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#2a2a2a;border-radius:24px;transition:.3s">
        <span id="manKnob" style="position:absolute;content:'';height:18px;width:18px;left:3px;bottom:3px;background:#555;border-radius:50%;transition:.3s"></span>
      </span>
    </label>
  </div>
</div>
<div id="manSliderWrap" style="display:none;margin-bottom:10px">
  <div class="fr">
    <label style="min-width:170px">Hitrost [%]</label>
    <div style="display:flex;align-items:center;gap:10px;flex:1">
      <input type="range" id="manPct" min="0" max="100" value="0"
             style="flex:1;accent-color:#00d4ff" oninput="onManSlider()">
      <span id="manPctVal" style="color:#00d4ff;font-size:14px;min-width:36px;text-align:right">0%</span>
    </div>
  </div>
</div>
<button class="btn" id="manApplyBtn" style="display:none" onclick="applyManual()">Nastavi</button>
<span class="msg" id="msgMan"></span>
</div></div>
<!-- TAB 2: KALIBRACIJA -->
<div class="pane" id="p2">
<div class="sec"><h3>SHT30 Kalibracija</h3>
<div class="fr"><label>Temp offset [°C]</label><input type="number" id="tOff" step="0.1"></div>
<div class="fr"><label>Hum offset [%]</label><input type="number" id="hOff" step="0.1"></div>
</div>
<div class="sec"><h3>INA219 Kalibracija</h3>
<div class="fr"><label>Shunt [Ω]</label><input type="number" id="sOhm" step="0.001" min="0.001"></div>
<div class="fr"><label>Korekcija toka</label><input type="number" id="cCorr" step="0.01" min="0.01"></div>
</div>
<div class="sec"><h3>WiFi</h3>
<div class="fr"><label>SSID</label><input type="text" id="wSsid" maxlength="31" style="width:200px"></div>
<div class="fr"><label>Geslo</label><input type="password" id="wPass" maxlength="63" style="width:200px"></div>
<button class="btn" onclick="saveCal()">Shrani</button><span class="msg" id="msgC"></span>
</div>
<div class="sec"><h3>Mini PC Monitor</h3>
<div class="fr"><label>IP naslov</label><input type="text" id="mIp" maxlength="15" style="width:140px"></div>
<div class="fr"><label>Prag porabe [W]</label><input type="number" id="mWth" step="0.1" min="0" style="width:80px"></div>
<div style="overflow-x:auto;margin:10px 0">
<table id="mPortTbl"><tr><th>Port</th><th>Ime</th><th>Enable</th></tr></table>
</div>
<button class="btn" onclick="saveMon()">Shrani monitor</button><span class="msg" id="msgM"></span>
</div></div>
<!-- TAB 3: SISTEM -->
<div class="pane" id="p3">
<div class="sec"><h3>Sistemske informacije</h3><div id="sysinfo"></div></div>
<div class="sec">
<h3>RAM Log &nbsp;<button class="btn bsm" onclick="fetchLog()">Osveži</button>&nbsp;<button class="btn bsm bto" onclick="clearLog()">Počisti</button>&nbsp;<a class="btn bsm" href="/api/log/download" download style="text-decoration:none">Download</a></h3>
<div style="overflow-x:auto;margin-top:10px">
<table id="logtbl"><tr><th>Čas</th><th>Level</th><th>Tag</th><th>Sporočilo</th></tr></table>
</div></div>
<div class="sec"><h3>OTA Firmware Update</h3>
<form id="otaF">
<input type="file" id="otaBin" accept=".bin" required style="display:block;width:100%;padding:8px;margin:8px 0 12px;background:#0d0d0d;border:2px dashed #333;border-radius:6px;color:#e0e0e0;cursor:pointer">
<button class="btn" type="submit">Naloži firmware</button>
</form>
<div id="otaPrg" style="display:none;width:100%;background:#2a2a2a;border-radius:4px;height:12px;margin-top:10px;overflow:hidden">
<div id="otaBar" style="height:100%;background:#00d4ff;width:0;transition:width 0.3s;border-radius:4px"></div></div>
<div id="otaSt" style="margin-top:8px;font-size:12px;color:#00d4ff"></div>
</div></div>
</main>
<link rel="stylesheet" href="/uplot.css">
<script src="/uplot.js"></script>
<script>
// Tab switching
let atab=0,_fL=false,_cL=false,_mL=false;
function sw(i){
  document.querySelectorAll('.t').forEach((e,n)=>e.classList.toggle('on',n===i));
  document.querySelectorAll('.pane').forEach((e,n)=>e.classList.toggle('on',n===i));
  atab=i;
  if(i===0 && !_uplot && _rawData) initUplot(_rawData);
  if(i===0) loadHistory();
  if(i===1&&!_fL)loadFan();
  if(i===2&&!_cL)loadCal();
  if(i===2&&!_mL)loadMon();
  if(i===3){fetchSys();fetchLog();}
}

function fUp(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return(h?h+'h ':'')+m+'m '+sec+'s';}

// Osveži kartice in grafe iz /api/data in /api/history
async function refreshData(){
  try{
    const d=await(await fetch('/api/data')).json();
    document.getElementById('hip').textContent=d.ip;
    document.getElementById('hup').textContent=fUp(d.uptime);
    document.getElementById('htim').textContent=d.time;
    document.getElementById('ct').textContent=d.temp.toFixed(1);
    document.getElementById('ch').textContent=d.hum.toFixed(1);
    document.getElementById('cv').textContent=d.volt.toFixed(2);
    document.getElementById('cw').textContent=d.watt.toFixed(1);
    document.getElementById('cf').textContent=d.fan;
    document.getElementById('cdnd').textContent=d.dnd?'DND aktiven':'';
    document.getElementById('cman').textContent=d.manual?'ROČNO':'';
    // Sinhronizacija sliderja če je tab 1 odprt
    if(atab===1){
      const tog=document.getElementById('manToggle');
      if(tog&&!tog.dataset.userEditing){
        tog.checked=d.manual;
        updateManUI(d.manual,d.manual_pct);
      }
    }
    document.getElementById('pkt').textContent=d.peak_temp>-900?'Peak: '+d.peak_temp.toFixed(1)+' °C':'';
    document.getElementById('pkw').textContent=d.peak_watt>0?'Peak: '+d.peak_watt.toFixed(1)+' W':'';
    if(atab===0)loadHistory();
  }catch(e){}
}

// ── uPlot graf ─────────────────────────────────────────────────────
const COLORS = {
  temp: '#e05252',
  hum:  '#5b9bd5',
  fan:  '#4ec9b0',
  watt: '#d4a76a',
  grid: getComputedStyle(document.documentElement)
          .getPropertyValue('--color-border-tertiary').trim() || '#2a2a2a',
  text: getComputedStyle(document.documentElement)
          .getPropertyValue('--color-text-secondary').trim() || '#888',
};

let _uplot = null;
let _rawData = null;
let _zoomed  = false;

function buildSeries() {
  const cbT = document.getElementById('cbTemp').checked;
  const cbH = document.getElementById('cbHum').checked;
  const cbF = document.getElementById('cbFan').checked;
  const cbW = document.getElementById('cbWatt').checked;

  const series = [{}];
  if (cbT) series.push({ label:'Temp °C', stroke:COLORS.temp, width:1.5,
    fill:COLORS.temp+'18', scale:'temp',
    value:(u,v)=>v==null?'--':v.toFixed(1)+' °C' });
  if (cbH) series.push({ label:'Vlaga %', stroke:COLORS.hum,  width:1.5,
    scale:'hum',
    value:(u,v)=>v==null?'--':v.toFixed(0)+' %' });
  if (cbF) series.push({ label:'Fan %',   stroke:COLORS.fan,  width:1.5,
    scale:'fan',
    value:(u,v)=>v==null?'--':v.toFixed(0)+' %' });
  if (cbW) series.push({ label:'Watt',    stroke:COLORS.watt, width:1.5,
    scale:'watt',
    value:(u,v)=>v==null?'--':v.toFixed(1)+' W' });
  return series;
}

function buildAxes() {
  const cbT = document.getElementById('cbTemp').checked;
  const cbH = document.getElementById('cbHum').checked;
  const cbF = document.getElementById('cbFan').checked;
  const cbW = document.getElementById('cbWatt').checked;
  const axStyle = { stroke:COLORS.text, grid:{stroke:COLORS.grid,width:0.5},
                    ticks:{show:false}, font:'10px monospace' };

  const axes = [{
    ...axStyle,
    values:(u,vs)=>vs.map(v=>{
      if(v==null)return'';
      const d=new Date(v*1000);
      return d.getHours().toString().padStart(2,'0')+':'+
             d.getMinutes().toString().padStart(2,'0');
    }),
  }];

  let leftDone = false;
  if (cbT) { axes.push({...axStyle,scale:'temp',stroke:COLORS.temp,
    label:'°C',size:42,side:leftDone?1:3}); if(!leftDone)leftDone=true; }
  if (cbH) { axes.push({...axStyle,scale:'hum', stroke:COLORS.hum,
    label:'%', size:42,side:leftDone?1:3,grid:{show:false}}); if(!leftDone)leftDone=true; }
  if (cbF) { axes.push({...axStyle,scale:'fan', stroke:COLORS.fan,
    label:'Fan%',size:48,side:1,grid:{show:false}}); }
  if (cbW) { axes.push({...axStyle,scale:'watt',stroke:COLORS.watt,
    label:'W',  size:42,side:1,grid:{show:false}}); }
  return axes;
}

function buildScales() {
  return {
    x:    {},
    temp: { range:(u,mn,mx)=>[mn-1, mx+1] },
    hum:  { range:(u,mn,mx)=>[Math.max(0,mn-3), Math.min(100,mx+3)] },
    fan:  { range:(u,mn,mx)=>[Math.max(0,mn-3), Math.min(100,mx+3)] },
    watt: { range:(u,mn,mx)=>[Math.max(0,mn-0.5), mx+0.5] },
  };
}

function buildData(raw) {
  if (!raw) return [[]];
  const cbT = document.getElementById('cbTemp').checked;
  const cbH = document.getElementById('cbHum').checked;
  const cbF = document.getElementById('cbFan').checked;
  const cbW = document.getElementById('cbWatt').checked;
  const out = [raw[0]];
  if (cbT) out.push(raw[1]);
  if (cbH) out.push(raw[2]);
  if (cbF) out.push(raw[3]);
  if (cbW) out.push(raw[4]);
  return out;
}

function getPlotWidth() {
  const wrap = document.getElementById('uplotWrap');
  return wrap ? Math.max(300, wrap.clientWidth) : 600;
}

function initUplot(raw) {
  _rawData = raw;
  const wrap = document.getElementById('uplotWrap');
  if (!wrap) return;
  wrap.innerHTML = '';

  const hasSeries = ['cbTemp','cbHum','cbFan','cbWatt']
    .some(id => document.getElementById(id).checked);
  if (!hasSeries) {
    wrap.innerHTML = '<div style="text-align:center;color:#555;padding:40px;font-size:12px">Izberi vsaj en podatek</div>';
    return;
  }

  const opts = {
    width:  getPlotWidth(),
    height: 220,
    cursor: {
      sync: { key: 'main' },
      drag: { x: true, y: false, setScale: false },
    },
    select: { show: true },
    legend: {
      show: true,
      live: true,
      markers: { show: true },
    },
    axes:   buildAxes(),
    scales: buildScales(),
    series: buildSeries(),
    hooks: {
      setSelect: [u => {
        const sel = u.select;
        if (sel.width > 10) {
          const xMin = u.posToVal(sel.left, 'x');
          const xMax = u.posToVal(sel.left + sel.width, 'x');
          u.setScale('x', { min: xMin, max: xMax });
          u.setSelect({ left:0, top:0, width:0, height:0 }, false);
          _zoomed = true;
          const btn = document.getElementById('zoomReset');
          if (btn) btn.style.display = '';
        }
      }],
      dblclick: [u => { resetZoom(); }],
    },
  };

  _uplot = new uPlot(opts, buildData(raw), wrap);
}

function resetZoom() {
  if (!_uplot || !_rawData) return;
  _uplot.setScale('x', { min: _rawData[0][0], max: _rawData[0][_rawData[0].length-1] });
  _zoomed = false;
  const btn = document.getElementById('zoomReset');
  if (btn) btn.style.display = 'none';
}

function rebuildUplot() {
  if (_rawData) initUplot(_rawData);
}

['cbTemp','cbHum','cbFan','cbWatt'].forEach(id => {
  const el = document.getElementById(id);
  if (el) el.addEventListener('change', rebuildUplot);
});

let _resizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(_resizeTimer);
  _resizeTimer = setTimeout(() => {
    if (_uplot) _uplot.setSize({ width: getPlotWidth(), height: 220 });
  }, 150);
});

async function loadHistory() {
  try {
    const pts = await (await fetch('/api/history')).json();
    if (!pts || pts.length === 0) return;

    const n    = pts.length;
    const xs   = new Float64Array(n);
    const temp = new Float64Array(n);
    const hum  = new Float64Array(n);
    const fan  = new Float64Array(n);
    const watt = new Float64Array(n);

    for (let i = 0; i < n; i++) {
      xs[i]   = pts[i].ts;
      temp[i] = pts[i].temp;
      hum[i]  = pts[i].hum;
      fan[i]  = pts[i].fan;
      watt[i] = pts[i].watt;
    }

    const raw = [xs, temp, hum, fan, watt];

    if (!_uplot) {
      initUplot(raw);
    } else if (!_zoomed) {
      _rawData = raw;
      _uplot.setData(buildData(raw));
    } else {
      _rawData = raw;
      const curMin = _uplot.scales.x.min;
      const curMax = _uplot.scales.x.max;
      _uplot.setData(buildData(raw));
      _uplot.setScale('x', { min: curMin, max: curMax });
    }
  } catch(e) {
    console.warn('loadHistory error:', e);
  }
}
// ── konec uPlot ────────────────────────────────────────────────────

setInterval(refreshData,5000);
refreshData();

// Monitor kartici + tabela portov (osveži vsakih 30s)
async function refreshMonitor(){
  try{
    const d=await(await fetch('/api/monitor')).json();
    document.getElementById('mpwr').textContent=d.powered?'ON':'OFF';
    document.getElementById('mpwr').style.color=d.powered?'#30d158':'#ff9500';
    document.getElementById('msvc').textContent=d.port_ok+'/'+d.port_total;
    document.getElementById('msvd').textContent=d.all_ok?'VSI OK':'NAPAKA';
    const box=document.getElementById('monBox');
    box.style.display='';
    const t=document.getElementById('monTbl');
    t.innerHTML='<tr><th>Port</th><th>Servis</th><th>Status</th></tr>';
    d.ports.forEach(p=>{
      if(!p.enabled)return;
      const tr=document.createElement('tr');
      tr.innerHTML=`<td>${p.port}</td><td>${p.name}</td><td>${p.ok?'&#10003;':'&#10007;'}</td>`;
      t.appendChild(tr);
    });
  }catch(e){}
}
setInterval(refreshMonitor,30000);
refreshMonitor();

// Naloži monitor nastavitve za Tab 2
async function loadMon(){
  try{
    const s=await(await fetch('/api/monitorsettings')).json();
    document.getElementById('mIp').value=s.monitorIp;
    document.getElementById('mWth').value=s.wattThreshold;
    const t=document.getElementById('mPortTbl');
    t.innerHTML='<tr><th>Port</th><th>Ime</th><th>Enable</th></tr>';
    s.ports.forEach((p,i)=>{
      const tr=document.createElement('tr');
      tr.innerHTML=`<td><input type="number" class="mp" data-i="${i}" value="${p.port}" min="0" max="65535" style="width:65px"></td><td><input type="text" class="mn" data-i="${i}" value="${p.name}" maxlength="11" style="width:80px"></td><td><input type="checkbox" class="me" data-i="${i}"${p.enabled?' checked':''}></td>`;
      t.appendChild(tr);
    });
    _mL=true;
  }catch(e){}
}

// Shrani monitor nastavitve
async function saveMon(){
  const ports=[];
  for(let i=0;i<8;i++){
    const pp=document.querySelector(`.mp[data-i="${i}"]`);
    const pn=document.querySelector(`.mn[data-i="${i}"]`);
    const pe=document.querySelector(`.me[data-i="${i}"]`);
    if(pp)ports.push({port:parseInt(pp.value)||0,name:pn?pn.value:'',enabled:pe?pe.checked:false});
  }
  const b={monitorIp:document.getElementById('mIp').value,wattThreshold:parseFloat(document.getElementById('mWth').value)||3.0,ports};
  const r=await fetch('/save/monitor',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgM');
  m.textContent=r.ok?'Shranjeno!':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
}

// Naloži nastavitve ventilatorja za Tab 1
async function loadFan(){
  try{
    const s=await(await fetch('/api/fansettings')).json();
    for(let i=0;i<4;i++){document.getElementById('ct'+i).value=s.curveTemp[i];document.getElementById('cp'+i).value=s.curvePct[i];}
    document.getElementById('fMin').value=s.fanMinPct;
    document.getElementById('fMaxD').value=s.fanMaxDayPct;
    document.getElementById('fDndM').value=s.dndMaxPct;
    document.getElementById('dndE').checked=s.dndEnabled;
    document.getElementById('dndF').value=s.dndFrom;
    document.getElementById('dndT').value=s.dndTo;
    // Naloži stanje ročnega načina
    try{
      const m=await(await fetch('/api/fan/manual')).json();
      document.getElementById('manToggle').checked=m.manual;
      document.getElementById('manPct').value=m.pct;
      document.getElementById('manPctVal').textContent=m.pct+'%';
      updateManUI(m.manual,m.pct);
    }catch(e){}
    _fL=true;
  }catch(e){}
}

// Naloži kalibracijske nastavitve za Tab 2
async function loadCal(){
  try{
    const s=await(await fetch('/api/calsettings')).json();
    document.getElementById('tOff').value=s.tempOffset;
    document.getElementById('hOff').value=s.humOffset;
    document.getElementById('sOhm').value=s.shuntOhms;
    document.getElementById('cCorr').value=s.currentCorr;
    document.getElementById('wSsid').value=s.ssid;
    _cL=true;
  }catch(e){}
}

// Shrani fan nastavitve
async function saveFan(){
  const ct=[0,1,2,3].map(i=>parseFloat(document.getElementById('ct'+i).value)||0);
  const cp=[0,1,2,3].map(i=>parseInt(document.getElementById('cp'+i).value)||0);
  const b={curveTemp:ct,curvePct:cp,
    fanMinPct:parseInt(document.getElementById('fMin').value)||0,
    fanMaxDayPct:parseInt(document.getElementById('fMaxD').value)||100,
    dndMaxPct:parseInt(document.getElementById('fDndM').value)||30,
    dndEnabled:document.getElementById('dndE').checked,
    dndFrom:parseInt(document.getElementById('dndF').value)||22,
    dndTo:parseInt(document.getElementById('dndT').value)||7};
  const r=await fetch('/save/fan',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgF');
  m.textContent=r.ok?'Shranjeno!':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
}

// Shrani kalibracijo + WiFi
async function saveCal(){
  const b={
    tempOffset:parseFloat(document.getElementById('tOff').value)||0,
    humOffset:parseFloat(document.getElementById('hOff').value)||0,
    shuntOhms:parseFloat(document.getElementById('sOhm').value)||0.1,
    currentCorr:parseFloat(document.getElementById('cCorr').value)||1,
    ssid:document.getElementById('wSsid').value,
    password:document.getElementById('wPass').value};
  const r=await fetch('/save/cal',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgC');
  m.textContent=r.ok?'Shranjeno! WiFi se posodobi ob resetu.':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',5000);
}

// Sistemske informacije (Tab 3)
async function fetchSys(){
  try{
    const d=await(await fetch('/api/data')).json();
    document.getElementById('sysinfo').innerHTML=
      `<div class="ir"><span class="ik">IP</span><span class="iv">${d.ip}</span></div>`+
      `<div class="ir"><span class="ik">RSSI</span><span class="iv">${d.rssi} dBm</span></div>`+
      `<div class="ir"><span class="ik">Uptime</span><span class="iv">${fUp(d.uptime)}</span></div>`+
      `<div class="ir"><span class="ik">Firmware</span><span class="iv">${d.fw}</span></div>`+
      `<div class="ir"><span class="ik">Free heap</span><span class="iv">${d.heap} B</span></div>`+
      `<div class="ir"><span class="ik">Napake</span><span class="iv"><span class="eb ${d.err===0?'eok':'efail'}">${d.err===0?'OK':'ERR 0x'+d.err.toString(16).toUpperCase()}</span></span></div>`;
  }catch(e){}
}

// RAM Log
async function fetchLog(){
  try{
    const logs=await(await fetch('/api/log')).json();
    const t=document.getElementById('logtbl');
    t.innerHTML='<tr><th>Čas</th><th>Level</th><th>Tag</th><th>Sporočilo</th></tr>';
    logs.forEach(e=>{
      const cls=e.level==='ERROR'?'le':e.level==='WARN'?'lw':'li';
      const tr=document.createElement('tr');tr.className=cls;
      tr.innerHTML=`<td>${e.time}</td><td>${e.level}</td><td>${e.tag}</td><td>${e.msg}</td>`;
      t.appendChild(tr);
    });
  }catch(e){}
}

async function clearLog(){
  try{await fetch('/api/log/clear',{method:'POST'});fetchLog();}catch(e){}
}

function updateManUI(isMan,pct){
  const lbl=document.getElementById('modeLabel');
  const wrap=document.getElementById('manSliderWrap');
  const btn=document.getElementById('manApplyBtn');
  const knob=document.getElementById('manKnob');
  const track=document.getElementById('manSliderToggle');
  if(isMan){
    lbl.textContent='ROČNO';lbl.style.color='#ff6b00';
    wrap.style.display='';btn.style.display='';
    track.style.background='#ff6b00';knob.style.left='23px';
  }else{
    lbl.textContent='AVTOMATSKO';lbl.style.color='#aaa';
    wrap.style.display='none';btn.style.display='none';
    track.style.background='#2a2a2a';knob.style.left='3px';
  }
  if(pct!==undefined){
    document.getElementById('manPct').value=pct;
    document.getElementById('manPctVal').textContent=pct+'%';
  }
}
function onManToggle(){
  const tog=document.getElementById('manToggle');
  tog.dataset.userEditing='1';
  updateManUI(tog.checked);
  if(!tog.checked){
    fetch('/api/fan/manual',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({manual:false,pct:0})})
    .then(()=>{
      const m=document.getElementById('msgMan');
      m.textContent='Avtomatsko';m.style.color='#30d158';
      setTimeout(()=>{m.textContent='';delete tog.dataset.userEditing;},2000);
    });
  }else{
    delete tog.dataset.userEditing;
  }
}
function onManSlider(){
  const v=document.getElementById('manPct').value;
  document.getElementById('manPctVal').textContent=v+'%';
}
async function applyManual(){
  const pct=parseInt(document.getElementById('manPct').value)||0;
  const r=await fetch('/api/fan/manual',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({manual:true,pct:pct})});
  const m=document.getElementById('msgMan');
  m.textContent=r.ok?'Nastavljeno '+pct+'%':'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
}

// OTA upload
document.getElementById('otaF').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('otaBin').files[0];if(!f)return;
  const btn=this.querySelector('button[type=submit]'),
        bar=document.getElementById('otaBar'),
        prg=document.getElementById('otaPrg'),
        st=document.getElementById('otaSt');
  btn.disabled=true;prg.style.display='block';st.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(ev){if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);bar.style.width=p+'%';st.textContent='Nalaganje: '+p+'%';}};
  xhr.onload=function(){if(xhr.status===200){bar.style.width='100%';bar.style.background='#30d158';st.textContent='Uspelo! Naprava se resetira v 5s...';setTimeout(()=>{location.href='/';},5500);}else{bar.style.background='#ff3b30';st.textContent='Napaka: '+xhr.responseText;btn.disabled=false;}};
  xhr.onerror=function(){st.textContent='Napaka pri prenosu!';btn.disabled=false;};
  const fd=new FormData();fd.append('update',f);
  xhr.open('POST','/update');xhr.send(fd);
};
</script></body></html>
)rawliteral";

// =====================================================================
// Pomožna: oblikuj čas kot HH:MM:SS
// =====================================================================
static void getTimeStr(char* buf, size_t sz) {
    if (timeSynced) {
        snprintf(buf, sz, "%02d:%02d:%02d", myTZ.hour(), myTZ.minute(), myTZ.second());
    } else {
        unsigned long s = millis() / 1000;
        snprintf(buf, sz, "%02lu:%02lu:%02lu", (s / 3600) % 24, (s / 60) % 60, s % 60);
    }
}

// =====================================================================
// initWebserver — WiFi, NTP, mDNS, registracija endpointov
// =====================================================================
void initWebserver() {
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        syncNTP();
        MDNS.begin(MDNS_HOSTNAME);
        LOG_INFO("MDNS", "http://%s.local", MDNS_HOSTNAME);
    }

    // --- GET / → glavna SPA stran ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html; charset=UTF-8", MAIN_HTML);
    });

    // --- GET /uplot.js → uPlot knjižnica iz PROGMEM ---
    server.on("/uplot.js", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *resp = request->beginResponse_P(
            200, "application/javascript", UPLOT_JS);
        resp->addHeader("Cache-Control", "public, max-age=86400");
        request->send(resp);
    });

    // --- GET /uplot.css → uPlot CSS iz PROGMEM ---
    server.on("/uplot.css", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *resp = request->beginResponse_P(
            200, "text/css", UPLOT_CSS);
        resp->addHeader("Cache-Control", "public, max-age=86400");
        request->send(resp);
    });

    // --- GET /api/data → JSON trenutnih vrednosti ---
    server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<768> doc;
        portENTER_CRITICAL(&dataMux);
        float temp = sensorData.temp, hum = sensorData.hum;
        float volt = sensorData.volt, amp = sensorData.amp, watt = sensorData.watt;
        float peakWattVal = sensorData.peakWatt;
        uint8_t fan = sensorData.fanPct;
        bool dnd = sensorData.dndActive;
        bool  manMode = sensorData.manualMode;
        uint8_t manPct = sensorData.manualPct;
        uint8_t err = sensorData.err;
        float outTemp = weatherData.outTemp;
        uint8_t outHum = weatherData.outHum;
        uint8_t wxCode = weatherData.wxCode;
        bool wxValid = weatherData.valid;
        portEXIT_CRITICAL(&dataMux);

        doc["temp"]      = serialized(String(temp, 1));
        doc["hum"]       = serialized(String(hum, 1));
        doc["volt"]      = serialized(String(volt, 2));
        doc["amp"]       = serialized(String(amp, 3));
        doc["watt"]      = serialized(String(watt, 1));
        doc["fan"]       = fan;
        doc["dnd"]        = dnd;
        doc["manual"]     = manMode;
        doc["manual_pct"] = manPct;
        doc["peak_temp"] = serialized(String(peakTemp, 1));
        doc["peak_watt"] = serialized(String(peakWattVal, 1));
        doc["out_temp"]  = serialized(String(outTemp, 1));
        doc["out_hum"]   = outHum;
        doc["wx_code"]   = wxCode;
        doc["wx_valid"]  = wxValid;

        char tbuf[10];
        getTimeStr(tbuf, sizeof(tbuf));
        doc["time"]   = tbuf;
        doc["uptime"] = millis() / 1000;
        doc["rssi"]   = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"]     = WIFI_STATIC_IP;
        doc["err"]    = err;
        doc["fw"]     = FW_VERSION;
        doc["heap"]   = ESP.getFreeHeap();

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/history → JSON array za grafe ---
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request){
        int cnt = graphGetCount();
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("[");
        for (int i = 0; i < cnt; i++) {
            GraphPoint p = graphGetPoint(i);
            if (i > 0) resp->print(",");
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "{\"ts\":%lu,\"temp\":%.1f,\"hum\":%.0f,\"watt\":%.1f,\"fan\":%u}",
                     (unsigned long)p.ts, p.temp, p.hum, p.watt, (unsigned)p.fanPct);
            resp->print(buf);
        }
        resp->print("]");
        request->send(resp);
    });

    // --- GET /api/log → JSON array zadnjih 50 vnosov ---
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request){
        int count = logGetCount();
        int start = (count > 50) ? count - 50 : 0;
        DynamicJsonDocument doc(32768);
        JsonArray arr = doc.to<JsonArray>();
        for (int i = start; i < count; i++) {
            LogEntry e = logGetEntry(i);
            JsonObject o = arr.createNestedObject();
            o["time"] = e.time;
            o["level"] = (e.level == LOG_LVL_ERROR) ? "ERROR" :
                         (e.level == LOG_LVL_WARN)  ? "WARN"  : "INFO";
            o["tag"]  = e.tag;
            o["msg"]  = e.msg;
        }
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /api/log/clear → počisti log buffer ---
    server.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest *request){
        logClear();
        LOG_INFO("WEB", "/api/log/clear");
        request->send(200, "application/json", "{\"status\":\"cleared\"}");
    });

    // --- GET /api/log/download → plaintext log za download z datumom v imenu ---
    server.on("/api/log/download", HTTP_GET, [](AsyncWebServerRequest *request){
        char fname[48];
        if (timeSynced) {
            snprintf(fname, sizeof(fname),
                     "fancontrol_log_%04d-%02d-%02d_%02d-%02d.txt",
                     myTZ.year(), myTZ.month(), myTZ.day(),
                     myTZ.hour(), myTZ.minute());
        } else {
            unsigned long s = millis() / 1000;
            snprintf(fname, sizeof(fname), "fancontrol_log_uptime_%lus.txt", s);
        }

        char disp[80];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);

        int count = logGetCount();
        AsyncResponseStream *resp = request->beginResponseStream("text/plain; charset=UTF-8");
        resp->addHeader("Content-Disposition", disp);
        char line[180];
        for (int i = 0; i < count; i++) {
            LogEntry e = logGetEntry(i);
            const char* lvl = (e.level == LOG_LVL_ERROR) ? "ERR" :
                              (e.level == LOG_LVL_WARN)  ? "WRN" : "INF";
            snprintf(line, sizeof(line), "[%s][%s][%s] %s\n",
                     e.time, lvl, e.tag, e.msg);
            resp->print(line);
        }
        request->send(resp);
        LOG_INFO("WEB", "/api/log/download — %d vnosov, ime: %s", count, fname);
    });

    // --- GET /api/fan/manual → vrni trenutno stanje ročnega načina ---
    server.on("/api/fan/manual", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<64> doc;
        doc["manual"] = isManualMode();
        doc["pct"]    = getManualPct();
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /api/fan/manual → nastavi ročni način ---
    server.on("/api/fan/manual", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char*  _manBodyBuf = nullptr;
            static size_t _manBodyLen = 0;
            if (index == 0) {
                if (!_manBodyBuf) _manBodyBuf = (char*)ps_malloc(256);
                if (!_manBodyBuf) { request->send(500, "text/plain", "OOM"); return; }
                _manBodyLen = 0;
                memset(_manBodyBuf, 0, 256);
            }
            size_t _manCopy = min(len, (size_t)(255 - _manBodyLen));
            memcpy(_manBodyBuf + _manBodyLen, data, _manCopy);
            _manBodyLen += _manCopy;
            if (index + len != total) return;

            StaticJsonDocument<64> doc;
            DeserializationError err = deserializeJson(doc, _manBodyBuf);
            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            bool    enable = doc["manual"] | false;
            uint8_t pct    = constrain((int)(doc["pct"] | 0), 0, 100);
            setManualMode(enable, pct);
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- GET /api/fansettings → za pre-fill Tab 1 ---
    server.on("/api/fansettings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<512> doc;
        JsonArray ct = doc.createNestedArray("curveTemp");
        JsonArray cp = doc.createNestedArray("curvePct");
        for (int i = 0; i < FAN_CURVE_POINTS; i++) {
            ct.add(settings.curveTemp[i]);
            cp.add(settings.curvePct[i]);
        }
        doc["fanMinPct"]    = settings.fanMinPct;
        doc["fanMaxDayPct"] = settings.fanMaxDayPct;
        doc["dndMaxPct"]    = settings.dndMaxPct;
        doc["dndEnabled"]   = settings.dndEnabled;
        doc["dndFrom"]      = settings.dndFrom;
        doc["dndTo"]        = settings.dndTo;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/calsettings → za pre-fill Tab 2 ---
    server.on("/api/calsettings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<256> doc;
        doc["tempOffset"]  = settings.tempOffset;
        doc["humOffset"]   = settings.humOffset;
        doc["shuntOhms"]   = settings.shuntOhms;
        doc["currentCorr"] = settings.currentCorr;
        doc["ssid"]        = settings.ssid;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /save/fan → shrani fan nastavitve v NVS ---
    server.on("/save/fan", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char* _fanBodyBuf = nullptr;
            static size_t _fanBodyLen = 0;
            if (index == 0) {
                if (!_fanBodyBuf) _fanBodyBuf = (char*)ps_malloc(2048);
                if (!_fanBodyBuf) { request->send(500, "text/plain", "OOM"); return; }
                _fanBodyLen = 0;
                memset(_fanBodyBuf, 0, 2048);
            }
            size_t copyLen = min(len, (size_t)(2047 - _fanBodyLen));
            memcpy(_fanBodyBuf + _fanBodyLen, data, copyLen);
            _fanBodyLen += copyLen;
            if (index + len != total) return;

            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, _fanBodyBuf);
            if (err) {
                LOG_ERROR("WEB", "/save/fan JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Validacija in vpis krivulje
            JsonArray ct = doc["curveTemp"];
            JsonArray cp = doc["curvePct"];
            if (ct.size() == FAN_CURVE_POINTS && cp.size() == FAN_CURVE_POINTS) {
                for (int i = 0; i < FAN_CURVE_POINTS; i++) {
                    float t = ct[i];
                    uint8_t p = cp[i];
                    if (t >= 0.0f && t <= 80.0f) settings.curveTemp[i] = t;
                    if (p <= 100) settings.curvePct[i] = p;
                }
            }
            if (doc.containsKey("fanMinPct")) {
                uint8_t v = doc["fanMinPct"];
                if (v <= 100) settings.fanMinPct = v;
            }
            if (doc.containsKey("fanMaxDayPct")) {
                uint8_t v = doc["fanMaxDayPct"];
                if (v <= 100) settings.fanMaxDayPct = v;
            }
            if (doc.containsKey("dndMaxPct")) {
                uint8_t v = doc["dndMaxPct"];
                if (v <= 100) settings.dndMaxPct = v;
            }
            if (doc.containsKey("dndEnabled")) settings.dndEnabled = doc["dndEnabled"];
            if (doc.containsKey("dndFrom")) {
                uint8_t v = doc["dndFrom"];
                if (v <= 23) settings.dndFrom = v;
            }
            if (doc.containsKey("dndTo")) {
                uint8_t v = doc["dndTo"];
                if (v <= 23) settings.dndTo = v;
            }

            saveSettings();
            LOG_INFO("WEB", "/save/fan OK");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- POST /save/cal → shrani kalibracija + WiFi v NVS ---
    server.on("/save/cal", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char* _calBodyBuf = nullptr;
            static size_t _calBodyLen = 0;
            if (index == 0) {
                if (!_calBodyBuf) _calBodyBuf = (char*)ps_malloc(2048);
                if (!_calBodyBuf) { request->send(500, "text/plain", "OOM"); return; }
                _calBodyLen = 0;
                memset(_calBodyBuf, 0, 2048);
            }
            size_t copyLen = min(len, (size_t)(2047 - _calBodyLen));
            memcpy(_calBodyBuf + _calBodyLen, data, copyLen);
            _calBodyLen += copyLen;
            if (index + len != total) return;

            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, _calBodyBuf);
            if (err) {
                LOG_ERROR("WEB", "/save/cal JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            if (doc.containsKey("tempOffset"))  settings.tempOffset  = doc["tempOffset"];
            if (doc.containsKey("humOffset"))    settings.humOffset   = doc["humOffset"];
            if (doc.containsKey("shuntOhms"))    settings.shuntOhms   = doc["shuntOhms"];
            if (doc.containsKey("currentCorr"))  settings.currentCorr = doc["currentCorr"];
            if (doc.containsKey("ssid")) {
                const char* v = doc["ssid"];
                if (v) strncpy(settings.ssid, v, sizeof(settings.ssid) - 1);
            }
            if (doc.containsKey("password")) {
                const char* v = doc["password"];
                if (v) strncpy(settings.password, v, sizeof(settings.password) - 1);
            }

            saveSettings();
            // WiFi sprememba velja ob resetu — ne reconnectamo takoj
            LOG_INFO("WEB", "/save/cal OK (WiFi posodobitev ob resetu)");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- GET /api/monitor → JSON statusov portov in napajanja ---
    server.on("/api/monitor", HTTP_GET, [](AsyncWebServerRequest *request){
        MonitorResult res = monitorGetResult();

        portENTER_CRITICAL(&dataMux);
        PortEntry portsCopy[MONITOR_MAX_PORTS];
        memcpy(portsCopy, monitorGetPorts(), sizeof(portsCopy));
        portEXIT_CRITICAL(&dataMux);

        StaticJsonDocument<1024> doc;
        doc["powered"]    = res.powered;
        doc["all_ok"]     = res.allPortsOk;
        doc["port_ok"]    = res.portOkCount;
        doc["port_total"] = res.portCount;
        JsonArray arr = doc.createNestedArray("ports");
        for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
            if (portsCopy[i].port == 0) continue;
            JsonObject o = arr.createNestedObject();
            o["port"]    = portsCopy[i].port;
            o["name"]    = portsCopy[i].name;
            o["enabled"] = portsCopy[i].enabled;
            o["ok"]      = portsCopy[i].lastOk;
        }
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/monitorsettings → za pre-fill Tab 2 ---
    server.on("/api/monitorsettings", HTTP_GET, [](AsyncWebServerRequest *request){
        char monIp[16];
        float wattThr;
        struct { uint16_t port; char name[12]; bool enabled; } pts[MONITOR_MAX_PORTS];

        portENTER_CRITICAL(&dataMux);
        memcpy(monIp, settings.monitorIp, sizeof(monIp));
        wattThr = settings.monitorWattThreshold;
        for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
            pts[i].port    = settings.monitorPorts[i].port;
            memcpy(pts[i].name, settings.monitorPorts[i].name, 12);
            pts[i].enabled = settings.monitorPorts[i].enabled;
        }
        portEXIT_CRITICAL(&dataMux);

        StaticJsonDocument<768> doc;
        doc["monitorIp"]     = monIp;
        doc["wattThreshold"] = wattThr;
        JsonArray arr = doc.createNestedArray("ports");
        for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
            JsonObject o = arr.createNestedObject();
            o["port"]    = pts[i].port;
            o["name"]    = pts[i].name;
            o["enabled"] = pts[i].enabled;
        }
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /save/monitor → shrani monitor nastavitve v NVS ---
    server.on("/save/monitor", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char* _monBodyBuf = nullptr;
            static size_t _monBodyLen = 0;
            if (index == 0) {
                if (!_monBodyBuf) _monBodyBuf = (char*)ps_malloc(2048);
                if (!_monBodyBuf) { request->send(500, "text/plain", "OOM"); return; }
                _monBodyLen = 0;
                memset(_monBodyBuf, 0, 2048);
            }
            size_t copyLen = min(len, (size_t)(2047 - _monBodyLen));
            memcpy(_monBodyBuf + _monBodyLen, data, copyLen);
            _monBodyLen += copyLen;
            if (index + len != total) return;

            StaticJsonDocument<1024> doc;
            DeserializationError err = deserializeJson(doc, _monBodyBuf);
            if (err) {
                LOG_ERROR("WEB", "/save/monitor JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc.containsKey("monitorIp")) {
                const char* v = doc["monitorIp"];
                if (v) strncpy(settings.monitorIp, v, sizeof(settings.monitorIp) - 1);
            }
            if (doc.containsKey("wattThreshold")) {
                float v = doc["wattThreshold"];
                if (v >= 0.0f && v <= 100.0f) settings.monitorWattThreshold = v;
            }
            JsonArray ports = doc["ports"];
            if (ports) {
                int idx = 0;
                for (JsonObject p : ports) {
                    if (idx >= MONITOR_MAX_PORTS) break;
                    settings.monitorPorts[idx].port    = p["port"] | 0;
                    const char* nm = p["name"] | "";
                    strncpy(settings.monitorPorts[idx].name, nm,
                            sizeof(settings.monitorPorts[idx].name) - 1);
                    settings.monitorPorts[idx].enabled = p["enabled"] | false;
                    idx++;
                }
            }
            saveSettings();
            monitorInit();
            LOG_INFO("WEB", "/save/monitor OK");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- GET /update → OTA HTML ---
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html; charset=UTF-8", OTA_HTML);
    });

    // --- POST /update → OTA flash (identično vent_SEW) ---
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest *request){
            bool ok = !Update.hasError();
            String msg = ok ? "OK" : Update.errorString();
            AsyncWebServerResponse *resp = request->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK" : ("FAIL: " + msg));
            resp->addHeader("Connection", "close");
            request->send(resp);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest *request, String filename,
           size_t index, uint8_t *data, size_t len, bool final){
            if (!index) {
                LOG_INFO("OTA", "Start: %s (%u B)",
                         filename.c_str(), request->contentLength());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                    LOG_ERROR("OTA", "begin failed: %s", Update.errorString());
            }
            if (!Update.hasError() && Update.write(data, len) != len)
                LOG_ERROR("OTA", "write failed");
            if (final) {
                if (Update.end(true))
                    LOG_INFO("OTA", "OK: %u B", index + len);
                else
                    LOG_ERROR("OTA", "end failed: %s", Update.errorString());
            }
        }
    );

    server.begin();
    LOG_INFO("WEB", "Server started on %s", WIFI_STATIC_IP);
}

// =====================================================================
// handleWebserver — mDNS + NTP re-sync + WiFi watchdog (klic v loop)
// =====================================================================
void handleWebserver() {
    // ESP32 ESPmDNS teče v ozadju — update() ni potreben

    // NTP re-sync vsakih 30 min
    if (timeSynced && millis() - lastNtpSyncMs >= NTP_UPDATE_INTERVAL) {
        syncNTP();
    }

    // WiFi watchdog vsakih 10 min
    if (millis() - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = millis();
        if (WiFi.status() != WL_CONNECTED) {
            LOG_WARN("WIFI", "Reconnecting...");
            connectWiFi();
            if (WiFi.status() == WL_CONNECTED && !timeSynced) syncNTP();
        }
    }
}
