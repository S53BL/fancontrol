// webserver.cpp — Web vmesnik za fancontrol
// Single-page app: Dashboard / Ventilator / Kalibracija / Sistem
// WiFi statični IP, NTP, mDNS, OTA flash

#include "webserver.h"
#include "globals.h"
#include "config.h"
#include "logging.h"
#include "fan_adapt.h"
#include "fan_boost.h"
#include "graph_store.h"
#include "fan.h"
#include "monitor.h"
#include "led.h"
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
#include "webserver_html_gz.h"

static AsyncWebServer server(80);

// =====================================================================
// WiFi Scan History — krožni buffer zadnjih WIFI_SCAN_HISTORY_SIZE eventov
// =====================================================================
enum WifiEventType : uint8_t {
    WIFI_EVENT_SCAN      = 0,  // periodični roaming scan
    WIFI_EVENT_RECONNECT = 1,  // health triggered reconnect
    WIFI_EVENT_MANUAL    = 2,  // ročni reconnect iz UI
    WIFI_EVENT_BOOT      = 3   // prvi connect ob zagonu
};

struct WifiScanEntry {
    uint32_t      ts;          // unix timestamp ali millis()/1000
    WifiEventType type;
    char          ssid[32];
    char          bssid[18];   // "AA:BB:CC:DD:EE:FF"
    int8_t        rssiPre;     // RSSI pred reconnectom (-120 če ni podatka)
    int8_t        rssiPost;    // RSSI po reconnectu
};

static WifiScanEntry wifiScanHistory[WIFI_SCAN_HISTORY_SIZE];
static uint8_t       wifiScanHistoryIdx   = 0;
static uint8_t       wifiScanHistoryCount = 0;

void wifiScanHistoryAdd(uint8_t type, const char* ssid, const char* bssid,
                        int rssiPre, int rssiPost) {
    WifiScanEntry& e = wifiScanHistory[wifiScanHistoryIdx];
    e.ts       = (uint32_t)(timeSynced ? time(nullptr) : millis() / 1000);
    e.type     = (WifiEventType)type;
    e.rssiPre  = (int8_t)rssiPre;
    e.rssiPost = (int8_t)rssiPost;
    strncpy(e.ssid,  ssid  ? ssid  : "", 31);  e.ssid[31]  = '\0';
    strncpy(e.bssid, bssid ? bssid : "", 17);  e.bssid[17] = '\0';
    wifiScanHistoryIdx = (wifiScanHistoryIdx + 1) % WIFI_SCAN_HISTORY_SIZE;
    if (wifiScanHistoryCount < WIFI_SCAN_HISTORY_SIZE) wifiScanHistoryCount++;
}

// =====================================================================
// WiFi Scan Rezultati — zadnji scan AP-jev v okolici
// =====================================================================
struct WifiApInfo {
    char    ssid[32];
    char    bssid[18];
    int8_t  rssi;
    uint8_t channel;
    bool    isConnected;
};

static WifiApInfo wifiScanResults[WIFI_SCAN_MAX_APS];
static uint8_t    wifiScanResultCount = 0;
static uint32_t   wifiLastScanMs      = 0;
static volatile bool wifiScanRequested    = false;
static volatile bool wifiScanInProgress   = false;
static volatile bool wifiConnectRequested = false;
static char          wifiConnectTarget[32] = "";

static void connectWiFi();  // forward — definicija pozneje v datoteki
static void syncNTP();      // forward — definicija pozneje v datoteki

// Poveži na specifičen SSID iz slota; ob neuspehu fallback na normalni connectWiFi()
static void wifiConnectToSSID(const char* targetSSID) {
    const char* pass = nullptr;
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        if (settings.wifiSlots[i].enabled &&
            strcmp(settings.wifiSlots[i].ssid, targetSSID) == 0) {
            pass = settings.wifiSlots[i].pass;
            break;
        }
    }
    if (!pass) {
        LOG_WARN("WIFI", "ConnectTo: '%s' ni v slotih", targetSSID);
        return;
    }
    LOG_INFO("WIFI", "ConnectTo: '%s'", targetSSID);
    int rssiPre = WiFi.RSSI();
    WiFi.disconnect();
    delay(500);
    WiFi.begin(targetSSID, pass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(300);
    if (WiFi.status() == WL_CONNECTED) {
        MDNS.end(); MDNS.begin(MDNS_HOSTNAME);
        if (!timeSynced) syncNTP();
        wifiScanHistoryAdd(WIFI_EVENT_MANUAL,
                           WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                           rssiPre, WiFi.RSSI());
        LOG_INFO("WIFI", "ConnectTo OK: '%s' %d dBm", targetSSID, WiFi.RSSI());
    } else {
        LOG_WARN("WIFI", "ConnectTo NEUSPELO — fallback reconnect...");
        connectWiFi();
        if (WiFi.status() == WL_CONNECTED) {
            MDNS.end(); MDNS.begin(MDNS_HOSTNAME);
            if (!timeSynced) syncNTP();
        }
    }
}

// Scan po posameznih kanalih — workaround za ESP-IDF bug kjer rec->primary
// vedno vrne connected channel namesto dejanskega kanala AP-ja.
// Kanal dobi iz scan config (ch), ne iz wifi_ap_record_t.primary.
static void wifiDoScan() {
    wifiScanResultCount = 0;
    wifiLastScanMs = millis();
    String connectedBSSID = WiFi.BSSIDstr();
    char bssidStr[18];

    for (uint8_t ch = 1; ch <= 13; ch++) {
        int n = WiFi.scanNetworks(false, true, false, 150, ch);
        if (n <= 0) { WiFi.scanDelete(); continue; }
        for (int i = 0; i < n && wifiScanResultCount < WIFI_SCAN_MAX_APS; i++) {
            wifi_ap_record_t *rec = (wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
            if (!rec) continue;
            snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     rec->bssid[0], rec->bssid[1], rec->bssid[2],
                     rec->bssid[3], rec->bssid[4], rec->bssid[5]);
            // Dedupliciraj — isti AP vidno na sosednjih kanalih (RF overlap 2.4GHz)
            bool dup = false;
            for (int j = 0; j < wifiScanResultCount; j++) {
                if (strcmp(wifiScanResults[j].bssid, bssidStr) == 0) {
                    if (rec->rssi > wifiScanResults[j].rssi) {
                        wifiScanResults[j].rssi    = rec->rssi;
                        wifiScanResults[j].channel = ch;
                    }
                    dup = true; break;
                }
            }
            if (!dup) {
                WifiApInfo& ap = wifiScanResults[wifiScanResultCount++];
                strncpy(ap.ssid, (const char*)rec->ssid, 31); ap.ssid[31] = '\0';
                strncpy(ap.bssid, bssidStr, 17); ap.bssid[17] = '\0';
                ap.rssi        = rec->rssi;
                ap.channel     = ch;
                ap.isConnected = (strcmp(bssidStr, connectedBSSID.c_str()) == 0);
            }
        }
        WiFi.scanDelete();
    }
    LOG_INFO("WIFI", "Scan done: %d AP-jev", wifiScanResultCount);
    for (int i = 0; i < wifiScanResultCount; i++)
        LOG_INFO("WIFI", "  [%d] %-20s ch=%2d %4d dBm", i, wifiScanResults[i].ssid,
                 wifiScanResults[i].channel, wifiScanResults[i].rssi);
}

// UI scan: disconnect → scan → reconnect (edina metoda za pravilne kanale)
// Kliči iz handleWebserver() — blokira main loop ~5-7s, ne AsyncWebServer
static void wifiUiScanAndReconnect() {
    LOG_INFO("WIFI", "UI scan: disconnect → scan → reconnect...");
    int rssiPre = WiFi.RSSI();
    WiFi.disconnect();
    delay(300);
    wifiDoScan();
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        MDNS.end();
        MDNS.begin(MDNS_HOSTNAME);
        wifiScanHistoryAdd(WIFI_EVENT_MANUAL,
                           WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                           rssiPre, WiFi.RSSI());
        LOG_INFO("WIFI", "UI scan done: %d AP-jev, reconnect OK %d dBm",
                 wifiScanResultCount, WiFi.RSSI());
    } else {
        LOG_WARN("WIFI", "UI scan done: %d AP-jev, reconnect NEUSPEŠEN",
                 wifiScanResultCount);
    }
}

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

    // Sunrise/sunset iz API — samo če je odgovor veljaven, sicer obdrži lokalni izračun
    if (srRaw && strlen(srRaw) >= 16) {
        strncpy(weatherData.sunrise, srRaw + 11, 5);
        weatherData.sunrise[5] = '\0';
        LOG_INFO("WX", "API sunrise: %s", weatherData.sunrise);
    }
    // else: ne prepiši z "--:--" — lokalni izračun iz calcSunTimes() ostane

    if (ssRaw && strlen(ssRaw) >= 16) {
        strncpy(weatherData.sunset, ssRaw + 11, 5);
        weatherData.sunset[5] = '\0';
        LOG_INFO("WX", "API sunset: %s", weatherData.sunset);
    }
    // else: ne prepiši z "--:--" — lokalni izračun iz calcSunTimes() ostane

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
// WiFi — statični IP, setScanMethod za boljši AP izbor v mesh okolju
// =====================================================================
static void connectWiFi() {
    WiFi.mode(WIFI_STA);

    // Scan method — izbere AP z najboljšim signalom (mesh optimizacija)
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    IPAddress ip, gw, sn, dns;
    ip.fromString(WIFI_STATIC_IP);
    gw.fromString(WIFI_STATIC_GW);
    sn.fromString(WIFI_STATIC_SUBNET);
    dns.fromString(WIFI_STATIC_DNS);
    WiFi.config(ip, gw, sn, dns);

    // Poskusi slote po vrstnem redu (vrstni red = prioriteta)
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        if (!settings.wifiSlots[i].enabled) continue;
        if (strlen(settings.wifiSlots[i].ssid) == 0) continue;

        LOG_INFO("WIFI", "Connecting [slot %d]: %s", i, settings.wifiSlots[i].ssid);
        WiFi.begin(settings.wifiSlots[i].ssid, settings.wifiSlots[i].pass);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(300);

        if (WiFi.status() == WL_CONNECTED) {
            LOG_INFO("WIFI", "Connected [slot %d]: %s  IP: %s  RSSI: %d dBm",
                     i, settings.wifiSlots[i].ssid,
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
            sensorData.err &= ~ERR_WIFI;
            wifiLastReconnectMs = millis();
            return;
        }
        WiFi.disconnect();
        delay(200);
    }

    LOG_WARN("WIFI", "Connect failed — noben slot ni dosegljiv");
    sensorData.err |= ERR_WIFI;
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
            wifiNtpFailCount = 0;  // uspelo — ponastavi fail counter
            LOG_INFO("NTP", "Synced [%s]: %s",
                     NTP_SERVER_LIST[i],
                     myTZ.dateTime("d.m.Y H:i:s").c_str());
            return;  // uspelo — ne nadaljuj z naslednjimi
        }
        LOG_WARN("NTP", "Timeout [%s] — naslednji...", NTP_SERVER_LIST[i]);
    }

    // Vsi strežniki neuspešni
    wifiNtpFailCount++;
    LOG_WARN("NTP", "Sync neuspesen — vsi strežniki brez odgovora (fail %d/%d)",
             wifiNtpFailCount, WIFI_NTP_FAIL_THRESHOLD);
    sensorData.err |= ERR_NTP;
}

// =====================================================================
// OTA HTML — identično vent_SEW, prilagojeno za fancontrol
// =====================================================================
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
.pbtn{padding:4px 10px;background:#1a1a1a;color:#555;border:1px solid #2a2a2a;border-radius:4px;cursor:pointer;font-family:monospace;font-size:11px;letter-spacing:1px;transition:color .15s,border-color .15s}
.pbtn:hover{color:#aaa;border-color:#444}
.pbtn.active{color:#00d4ff;border-color:#00d4ff;background:#00d4ff11}
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
/* Toggle switch */
.tgsw{position:relative;display:inline-block;width:44px;height:24px;flex-shrink:0}
.tgsw input{opacity:0;width:0;height:0}
.tgtr{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#2a2a2a;border-radius:24px;transition:.3s}
.tgkn{position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:#555;border-radius:50%;transition:.3s}
/* Sistem 2-kolona */
.sys2{display:flex;gap:12px;align-items:flex-start}
.sys2 .slog{flex:1.4;min-width:0}
.sys2 .sinf{flex:1;min-width:200px}
@media(max-width:600px){.sys2{flex-direction:column}}
.logbox{height:400px;overflow-y:auto;background:#0d0d0d;border:1px solid #1e1e1e;border-radius:4px;margin-top:8px}
.logbox table{width:100%}
.logflt{display:flex;gap:12px;margin-bottom:8px;flex-wrap:wrap}
.logflt label{font-size:11px;color:#aaa;display:flex;align-items:center;gap:4px;cursor:pointer}
/* Fan curve popup */
#curveEditPopup input:focus{border-color:#00d4ff}
#fanCurveSec{position:relative}
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
<div class="t" onclick="sw(4)">WIFI</div>
</nav>
<main>

<!-- ═══════════════════════════════════════════════════ TAB 0: DASHBOARD -->
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
<!-- Graf — PRED servisi -->
<div class="cw">
  <!-- Vrstica 1: naslov + kontrole desno -->
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;flex-wrap:wrap;gap:6px">
    <div class="ct2" style="margin:0">GRAF ZGODOVINE</div>
    <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">
      <button class="btn bsm" id="btnLive" onclick="gotoLive()" style="background:#30d158;color:#0d0d0d">LIVE</button>
      <button class="btn bsm" id="btnReset" onclick="resetGraph()">RESET</button>
      <button class="btn bsm" onclick="downloadCSV()" style="background:#555;color:#e0e0e0">&#11015; CSV</button>
    </div>
  </div>
  <!-- Vrstica 2: period gumbi -->
  <div style="display:flex;gap:5px;margin-bottom:8px;flex-wrap:wrap" id="periodBtns">
    <button class="pbtn" data-sec="600" onclick="setPeriod(600)">10min</button>
    <button class="pbtn active" data-sec="3600" onclick="setPeriod(3600)">1h</button>
    <button class="pbtn" data-sec="21600" onclick="setPeriod(21600)">6h</button>
    <button class="pbtn" data-sec="86400" onclick="setPeriod(86400)">24h</button>
    <button class="pbtn" data-sec="259200" onclick="setPeriod(259200)">3d</button>
    <button class="pbtn" data-sec="604800" onclick="setPeriod(604800)">7d</button>
    <button class="pbtn" data-sec="0" onclick="setPeriod(0)">VSE</button>
  </div>
  <!-- Vrstica 3: series checkboxi -->
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
  <!-- Graf -->
  <div id="uplotWrap" style="width:100%"></div>
  <!-- Višina slider -->
  <div style="display:flex;align-items:center;gap:8px;margin-top:8px">
    <span style="font-size:10px;color:#555;white-space:nowrap">Višina grafa:</span>
    <input type="range" id="graphHeight" min="180" max="600" value="320"
           style="flex:1;accent-color:#00d4ff;max-width:160px" oninput="onGraphHeightChange()">
    <span id="graphHeightVal" style="font-size:10px;color:#555;min-width:32px">320px</span>
  </div>
</div>
<!-- Servisi — POD grafom -->
<div id="monBox" class="cw" style="display:none">
<div class="ct2">Server Servisi</div>
<table id="monTbl"><tr><th>Port</th><th>Servis</th><th>Status</th></tr></table>
</div>
</div>

<!-- ═══════════════════════════════════════════════════ TAB 1: VENTILATOR -->
<div class="pane" id="p1">
<!-- Krivulja ventilatorja — SVG graf -->
<div class="sec" id="fanCurveSec" style="margin-bottom:12px">
<h3>KRIVULJA VENTILATORJA</h3>
<div style="position:relative;width:100%">
  <svg id="fanCurveSvg" viewBox="0 0 520 220" style="width:100%;display:block;background:#0d0d0d;border-radius:6px;overflow:visible"></svg>
</div>
<div style="display:flex;align-items:center;gap:10px;margin-top:10px">
  <button class="btn" onclick="saveFan('msgF1')">Shrani</button>
  <span class="msg" id="msgF1"></span>
</div>
<!-- Legenda -->
<div style="display:flex;flex-wrap:wrap;gap:16px;margin-top:8px;font-size:10px;color:#888;padding:0 4px">
  <span><svg width="28" height="8" style="vertical-align:middle"><line x1="0" y1="4" x2="28" y2="4" stroke="#444" stroke-width="1.5" stroke-dasharray="4,3"/></svg> Default krivulja</span>
  <span><svg width="28" height="8" style="vertical-align:middle"><line x1="0" y1="4" x2="28" y2="4" stroke="#00d4ff" stroke-width="2"/></svg> Aktivna krivulja</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><circle cx="6" cy="6" r="5" fill="#00d4ff"/></svg> Prosta točka</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><circle cx="6" cy="6" r="5" fill="#ff9500"/></svg> Zaklenjena točka</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><circle cx="6" cy="6" r="5" fill="none" stroke="#555" stroke-width="1.5"/></svg> Nizek confidence</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><circle cx="6" cy="6" r="5" fill="none" stroke="#ffd700" stroke-width="1.5"/></svg> Srednji confidence</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><circle cx="6" cy="6" r="5" fill="none" stroke="#30d158" stroke-width="1.5"/></svg> Visok confidence</span>
  <span><svg width="6" height="14" style="vertical-align:middle"><line x1="3" y1="0" x2="3" y2="14" stroke="#e05252" stroke-width="2"/></svg> Trenutna temp</span>
  <span><svg width="12" height="12" style="vertical-align:middle"><polygon points="6,0 12,10 0,10" fill="#ff9500"/></svg> Watt boost</span>
</div>
<!-- Inline edit popup (skrit) -->
<div id="curveEditPopup" style="display:none;position:absolute;z-index:100;background:#1a1a1a;border:1px solid #00d4ff;border-radius:6px;padding:10px 14px;font-size:12px;box-shadow:0 4px 16px #000a">
  <div style="color:#555;font-size:10px;margin-bottom:6px" id="editPopupLabel">Točka 0</div>
  <div style="display:flex;flex-direction:column;gap:6px">
    <div style="display:flex;align-items:center;gap:8px">
      <span style="color:#aaa;min-width:36px">Temp:</span>
      <input type="number" id="editPopupTemp" min="0" max="80" step="0.5"
        style="width:64px;background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:3px 6px;border-radius:4px;font-family:monospace">
      <span style="color:#555;font-size:10px">°C</span>
    </div>
    <div style="display:flex;align-items:center;gap:8px">
      <span style="color:#aaa;min-width:36px">Fan%:</span>
      <input type="number" id="editPopupInput" min="0" max="100"
        style="width:64px;background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:3px 6px;border-radius:4px;font-family:monospace">
      <span style="color:#555;font-size:10px">%</span>
    </div>
    <div style="display:flex;gap:6px;margin-top:2px">
      <button onclick="editPopupApply()" style="flex:1;padding:4px 10px;background:#00d4ff;color:#0d0d0d;border:none;border-radius:4px;cursor:pointer;font-weight:bold;font-size:11px">OK</button>
      <button onclick="editPopupClose()" style="padding:4px 10px;background:#2a2a2a;color:#aaa;border:none;border-radius:4px;cursor:pointer;font-size:11px">&#x2715;</button>
    </div>
  </div>
  <div style="font-size:10px;color:#ff3b30;margin-top:4px;display:none" id="editPopupErr"></div>
  <div style="font-size:10px;color:#ff9500;margin-top:2px" id="editPopupLockWarn"></div>
</div>
</div>
<!-- Ročno upravljanje — PRVI blok -->
<div class="sec"><h3>Ročno upravljanje</h3>
<div class="fr" style="margin-bottom:14px">
  <label style="min-width:170px">Način</label>
  <div style="display:flex;align-items:center;gap:10px;max-width:280px">
    <span id="modeLabel" style="font-size:12px;color:#aaa;min-width:90px">AVTOMATSKO</span>
    <label class="tgsw">
      <input type="checkbox" id="manToggle" onchange="onManToggle()">
      <span class="tgtr" id="manSliderToggle"><span class="tgkn" id="manKnob"></span></span>
    </label>
  </div>
</div>
<div id="manSliderWrap" style="display:none;margin-bottom:10px">
  <div class="fr">
    <label style="min-width:170px">Hitrost [%]</label>
    <div style="display:flex;align-items:center;gap:10px;max-width:280px;flex:1">
      <input type="range" id="manPct" min="0" max="100" value="0"
             style="flex:1;accent-color:#00d4ff" oninput="onManSlider()">
      <span id="manPctVal" style="color:#00d4ff;font-size:14px;min-width:36px;text-align:right">0%</span>
    </div>
  </div>
</div>
<button class="btn" id="manApplyBtn" style="display:none" onclick="applyManual()">Nastavi</button>
<span class="msg" id="msgMan"></span>
</div>
<!-- Temperaturna krivulja -->
<div class="sec"><h3>Temperaturna krivulja</h3>
<div style="display:flex;align-items:center;gap:10px;margin-bottom:10px">
  <button class="btn" onclick="saveFan('msgF3')">Shrani</button>
  <span class="msg" id="msgF3"></span>
</div>
<table class="ctbl" style="margin-bottom:14px">
<tr><th>Točka</th><th>Opis</th><th>Temp [°C]</th><th>Fan [%]</th><th>Conf</th><th>Zakleni</th></tr>
<tr><td>0</td><td style="color:#888;font-size:10px">Mirovanje</td><td><input type="number" id="ct0" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp0" min="0" max="100" style="width:60px"></td><td><span id="cc0" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl0" style="accent-color:#ff9500"></td></tr>
<tr><td>1</td><td style="color:#888;font-size:10px">Lahka obremenitev</td><td><input type="number" id="ct1" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp1" min="0" max="100" style="width:60px"></td><td><span id="cc1" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl1" style="accent-color:#ff9500"></td></tr>
<tr><td>2</td><td style="color:#888;font-size:10px">Zmerna obremenitev</td><td><input type="number" id="ct2" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp2" min="0" max="100" style="width:60px"></td><td><span id="cc2" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl2" style="accent-color:#ff9500"></td></tr>
<tr><td>3</td><td style="color:#888;font-size:10px">Višja obremenitev</td><td><input type="number" id="ct3" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp3" min="0" max="100" style="width:60px"></td><td><span id="cc3" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl3" style="accent-color:#ff9500"></td></tr>
<tr><td>4</td><td style="color:#888;font-size:10px">Visoka obremenitev</td><td><input type="number" id="ct4" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp4" min="0" max="100" style="width:60px"></td><td><span id="cc4" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl4" style="accent-color:#ff9500"></td></tr>
<tr><td>5</td><td style="color:#888;font-size:10px">Maksimum / zaščita</td><td><input type="number" id="ct5" min="0" max="80" step="0.5" style="width:70px"></td><td><input type="number" id="cp5" min="0" max="100" style="width:60px"></td><td><span id="cc5" style="color:#00d4ff;font-size:11px">--</span></td><td><input type="checkbox" id="cl5" style="accent-color:#ff9500"></td></tr>
</table>
<div style="margin-bottom:12px;font-size:10px;color:#555">
  Confidence: število ravnovesnih opazovanj (prag za aktivacijo: 20).
  Zaklenjene točke algoritem ne posodablja.
</div>
<div style="display:flex;justify-content:flex-end;margin-bottom:8px">
  <button class="btn bsm bto" onclick="adaptReset()"
    style="font-size:10px;padding:3px 10px;opacity:0.7">↺ Reset učenja</button>
</div>
</div>
<!-- Watt Boost -->
<div class="sec"><h3>Watt Feed-Forward Boost</h3>
<p style="font-size:11px;color:#555;margin-bottom:12px">
  Ko poraba preseže prag, se fan% takoj poveča za boost vrednost.
  Sistem oceni rezultat po nastavljenem času in se samodejno kalibrira.
</p>
<div class="fr"><label>Prag obremenitve [W]</label><input type="number" id="bWth" min="1" max="50" step="0.5" style="width:80px"></div>
<div class="fr"><label>Zakleni boost% (ne uči se)</label>
  <input type="checkbox" id="bLock" style="accent-color:#ff9500" onchange="onBoostLockChange()">
</div>
<div class="fr"><label>Boost vrednost [%]</label>
  <input type="number" id="bPct" min="5" max="40" style="width:70px">
  &nbsp;<span id="bPctConf" style="font-size:11px;font-weight:bold;padding:2px 7px;border-radius:3px"></span>
  <span style="font-size:10px;color:#555;margin-left:6px">privzeto: 10%</span>
</div>
<div class="fr"><label>Fade [s]</label><input type="number" id="bEval" min="5" max="300" style="width:70px"></div>
<div class="fr"><label>Eval okno [s]</label><input type="number" id="bLearn" min="30" max="600" style="width:70px" title="Čas po aktivaciji boosta za oceno temperaturnega trenda (samoučenje)"></div>
<div class="fr"><label>Status</label><span id="bStatus" style="font-size:12px;color:#555">--</span></div>
<div style="display:flex;align-items:center;gap:10px;margin-top:8px">
  <button class="btn" onclick="saveFan('msgF4')">Shrani</button>
  <span class="msg" id="msgF4"></span>
</div>
</div>
<!-- Limiti in DND -->
<div class="sec"><h3>Limiti in DND</h3>
<div class="fr"><label>Zagon ventilatorja [%pwm]</label><input type="number" id="fanStartPct" min="1" max="100" style="width:70px"></div>
<div class="fr"><label>Izklop ventilatorja [%pwm]</label><input type="number" id="fanStopPct" min="0" max="99" style="width:70px"></div>
<div class="fr"><label>Max podnevi [%]</label><input type="number" id="fMaxD" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>Max DND [%]</label><input type="number" id="fDndM" min="0" max="100" step="1" style="width:70px"></div>
<div style="font-size:10px;color:#555;margin-bottom:8px;margin-left:180px">
  0% = fan ugasnjen med DND &nbsp;|&nbsp; ali vsaj toliko kot prag zagona
</div>
<div class="fr"><label>DND omogočen</label><input type="checkbox" id="dndE"></div>
<div class="fr"><label>DND od ure (0–23)</label><input type="number" id="dndF" min="0" max="23" style="width:70px"></div>
<div class="fr"><label>DND do ure (0–23)</label><input type="number" id="dndT" min="0" max="23" style="width:70px"></div>
<button class="btn" onclick="saveFan('msgF5')">Shrani</button>
<span class="msg" id="msgF5"></span>
</div>
</div>

<!-- ═══════════════════════════════════════════════════ TAB 2: KALIBRACIJA -->
<div class="pane" id="p2">
<div class="sec"><h3>SHT30 Kalibracija</h3>
<div class="fr"><label>Temp offset [°C]</label><input type="number" id="tOff" step="0.1"></div>
<div class="fr"><label>Hum offset [%]</label><input type="number" id="hOff" step="0.1"></div>
</div>
<div class="sec"><h3>INA219 Kalibracija</h3>
<div class="fr"><label>Shunt [Ω]</label><input type="number" id="sOhm" step="0.001" min="0.001"></div>
<div class="fr"><label>Korekcija toka</label><input type="number" id="cCorr" step="0.01" min="0.01"></div>
</div>
<div style="margin-bottom:8px">
<button class="btn" onclick="saveCal()">Shrani</button><span class="msg" id="msgC"></span>
</div>
<!-- Server Monitor -->
<div class="sec"><h3>Server Monitor</h3>
<div class="fr"><label>IP naslov</label><input type="text" id="mIp" maxlength="15" style="width:140px"></div>
<div class="fr"><label>Prag porabe [W]</label><input type="number" id="mWth" step="0.1" min="0" style="width:80px"></div>
<div style="overflow-x:auto;margin:10px 0">
<table id="mPortTbl"><tr><th>Port</th><th>Ime</th><th>Enable</th></tr></table>
</div>
<button class="btn" onclick="saveMon()">Shrani monitor</button><span class="msg" id="msgM"></span>
</div>
<!-- LED stikalo -->
<div class="sec"><h3>RGB LED</h3>
<div class="fr">
  <label style="min-width:170px">LED statusna lučka</label>
  <div style="display:flex;align-items:center;gap:10px">
    <span id="ledLabel" style="font-size:12px;color:#aaa;min-width:70px">--</span>
    <label class="tgsw">
      <input type="checkbox" id="ledToggle" onchange="onLedToggle()">
      <span class="tgtr" id="ledTrack"><span class="tgkn" id="ledKnob"></span></span>
    </label>
  </div>
</div>
<span class="msg" id="msgLed"></span>
</div>
<!-- PWM kalibracija -->
<div class="sec"><h3>PWM Kalibracija</h3>
<div class="fr">
  <label style="min-width:170px">Frekvenca [Hz]</label>
  <input type="number" id="pwmFreq" min="10" max="50000" step="1" style="width:100px">
  <span style="font-size:10px;color:#555;margin-left:8px">10 – 50000 Hz &nbsp;|&nbsp; priporočeno: 25000</span>
</div>
<div class="fr">
  <label style="min-width:170px">Invert delovni cikel</label>
  <div style="display:flex;align-items:center;gap:10px">
    <span id="pwmInvLabel" style="font-size:12px;color:#aaa;min-width:70px">--</span>
    <label class="tgsw">
      <input type="checkbox" id="pwmInvToggle" onchange="onPwmInvToggle()">
      <span class="tgtr" id="pwmInvTrack"><span class="tgkn" id="pwmInvKnob"></span></span>
    </label>
  </div>
</div>
<div style="margin-bottom:8px;font-size:10px;color:#555">
  Invert: HIGH=stop, LOW=teče. Normalno (off): LOW=stop, HIGH=teče.
</div>
<div style="display:flex;align-items:center;gap:10px">
  <button class="btn" onclick="savePwmCal()">Shrani in aktiviraj</button>
  <span class="msg" id="msgPwm"></span>
</div>
</div>
</div>

<!-- ═══════════════════════════════════════════════════ TAB 3: SISTEM -->
<div class="pane" id="p3">
<div class="sys2">
  <!-- LEVO: RAM Log -->
  <div class="slog">
    <div class="sec" style="height:100%">
      <h3>RAM Log
        &nbsp;<button class="btn bsm" onclick="fetchLog()">Osveži</button>
        &nbsp;<button class="btn bsm bto" onclick="clearLog()">Počisti</button>
        &nbsp;<button class="btn bsm" onclick="downloadLog()" style="background:#555;color:#e0e0e0">&#11015; Download</button>
      </h3>
      <div class="logflt">
        <label><input type="checkbox" id="fInfo" checked onchange="applyLogFilter()"> <span style="color:#888">INFO</span></label>
        <label><input type="checkbox" id="fWarn" checked onchange="applyLogFilter()"> <span style="color:#ff9500">WARN</span></label>
        <label><input type="checkbox" id="fErr"  checked onchange="applyLogFilter()"> <span style="color:#ff3b30">ERROR</span></label>
      </div>
      <div class="logbox" id="logbox">
        <table id="logtbl"><tr><th>Čas</th><th>Lvl</th><th>Tag</th><th>Sporočilo</th></tr></table>
      </div>
    </div>
  </div>
  <!-- DESNO: Sistemske informacije -->
  <div class="sinf">
    <div class="sec">
      <h3>Sistem</h3>
      <div id="sysinfo"></div>
    </div>
    <!-- OTA -->
    <div class="sec" style="margin-top:12px">
      <h3>OTA Update</h3>
      <form id="otaF">
      <input type="file" id="otaBin" accept=".bin" required style="display:block;width:100%;padding:8px;margin:8px 0 12px;background:#0d0d0d;border:2px dashed #333;border-radius:6px;color:#e0e0e0;cursor:pointer">
      <button class="btn" type="submit">Naloži firmware</button>
      </form>
      <div id="otaPrg" style="display:none;width:100%;background:#2a2a2a;border-radius:4px;height:12px;margin-top:10px;overflow:hidden">
      <div id="otaBar" style="height:100%;background:#00d4ff;width:0;transition:width 0.3s;border-radius:4px"></div></div>
      <div id="otaSt" style="margin-top:8px;font-size:12px;color:#00d4ff"></div>
    </div>
    <!-- Ponovni zagon -->
    <div class="sec" style="margin-top:12px">
      <h3>Ponovni zagon</h3>
      <p style="font-size:11px;color:#555;margin-bottom:12px">Naprava se bo restartirala. Stran se bo samodejno obnovila čez 6 sekund.</p>
      <button class="btn bto" onclick="doRestart()">&#128260; Ponovni zagon</button>
      <span class="msg" id="msgRestart"></span>
    </div>
    <!-- Ponastavitev nastavitev -->
    <div class="sec" style="margin-top:12px;border:1px solid #3a1a1a;border-radius:6px;padding:12px">
      <h3 style="color:#ff3b30">Ponastavitev nastavitev</h3>
      <p style="font-size:11px;color:#555;margin-bottom:12px">Izbriše vse shranjene nastavitve (krivulja, kalibracija, DND, Boost...) in zapiše privzete vrednosti. WiFi nastavitve ostanejo. Naprava se samodejno restartira.</p>
      <button class="btn bto" style="border-color:#ff3b30;color:#ff3b30" onclick="doSettingsReset()">&#9888; Ponastavi na privzete vrednosti</button>
      <span class="msg" id="msgSettingsReset"></span>
    </div>
  </div>
</div>
</div>

<!-- ═══════════════════════════════════════════════════ TAB 4: WIFI -->
<div class="pane" id="p4">

<!-- Razdelek 1: Trenutno stanje + Bar Chart -->
<div class="sec">
  <h3>WIFI STANJE
    &nbsp;<button class="btn bsm" onclick="wifiScanNow(this)" title="Kratka prekinitev ~6s (disconnect→scan→reconnect)">Skeniraj zdaj</button>
    &nbsp;<button class="btn bsm bto" onclick="wifiReconnectNow()">Reconnect</button>
  </h3>
  <div id="wifiCurrentInfo" style="margin-bottom:12px">
    <div class="ir"><span class="ik">SSID</span><span class="iv" id="wCurrentSsid">--</span></div>
    <div class="ir"><span class="ik">BSSID (Access Point)</span><span class="iv" id="wCurrentBssid">--</span></div>
    <div class="ir"><span class="ik">Kanal</span><span class="iv" id="wCurrentCh">--</span></div>
    <div class="ir"><span class="ik">RSSI</span><span class="iv" id="wCurrentRssi">--</span></div>
    <div class="ir"><span class="ik">Zadnji scan</span><span class="iv" id="wLastScan">--</span></div>
  </div>
  <div id="wifiBarChart" style="width:100%;overflow-x:auto"></div>
</div>

<!-- Razdelek 2: History -->
<div class="sec">
  <h3>HISTORY (zadnjih 10 eventov)</h3>
  <div style="overflow-x:auto">
  <table id="wifiHistTbl" style="width:100%;font-size:11px">
    <tr><th>Čas</th><th>Event</th><th>SSID</th><th>BSSID</th><th>RSSI pred</th><th>RSSI po</th></tr>
  </table>
  </div>
</div>

<!-- Razdelek 3: Seznam omrežij (5 slotov) -->
<div class="sec">
  <h3>OMREŽJA</h3>
  <p style="font-size:11px;color:#555;margin-bottom:12px">
    Vrstni red = prioriteta. Vsaj eno omrežje mora biti aktivno.
    Prednastavljena gesla niso vidna — pusti prazno da ohranišobstoječe.
  </p>
  <div id="wifiSlotsList"></div>
  <button class="btn" onclick="saveWifiSlots()" style="margin-top:10px">Shrani omrežja</button>
  <span class="msg" id="msgWifi"></span>
  <div style="margin-top:12px;border-top:1px solid #2a2a2a;padding-top:12px">
    <button class="btn bsm" style="opacity:0.6;font-size:10px" onclick="wifiFactoryReset()">&#8635; Ponastavi na privzeta omrežja</button>
  </div>
</div>

</div>

</main>
<link rel="stylesheet" href="/uplot.css">
<script src="/uplot.js"></script>
<script>
// ── Tab switching ──────────────────────────────────────────────────
let atab=0,_fL=false,_cL=false,_mL=false,_wL=false;
function sw(i){
  document.querySelectorAll('.t').forEach((e,n)=>e.classList.toggle('on',n===i));
  document.querySelectorAll('.pane').forEach((e,n)=>e.classList.toggle('on',n===i));
  atab=i;
  if(i===0&&!_uplot&&_rawData)initUplot(_rawData);
  if(i===0)loadHistory();
  if(i===1){if(!_fL)loadFan();else drawFanCurveFromCache();}
  if(i===2&&!_cL)loadCal();
  if(i===2&&!_mL)loadMon();
  if(i===2)loadLed();
  if(i===3){fetchSys();fetchLog();}
  if(i===4)loadWifi();
}

function fUp(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return(h?h+'h ':'')+m+'m '+sec+'s';}
function fBytes(b){if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(0)+' KB';return b+' B';}
function fPct(free,total){return total>0?Math.round(free/total*100)+'%':'--';}

// ── Autorefresh 5s ─────────────────────────────────────────────────
async function refreshData(){
  try{
    const d=await(await fetch('/api/data')).json();
    document.getElementById('hip').textContent=d.ip;
    document.getElementById('hup').textContent=fUp(d.uptime);
    document.getElementById('htim').textContent=d.time;
    document.getElementById('ct').textContent=d.temp;
    document.getElementById('ch').textContent=d.hum;
    document.getElementById('cv').textContent=d.volt;
    document.getElementById('cw').textContent=d.watt;
    document.getElementById('cf').textContent=d.fan;
    document.getElementById('cdnd').textContent=d.dnd?'DND aktiven':'';
    document.getElementById('cman').textContent=d.manual?'ROČNO':'';
    if(atab===1){
      const tog=document.getElementById('manToggle');
      if(tog)updateManUI(d.manual,d.manual_pct);
    }
    document.getElementById('pkt').textContent=d.peak_temp>-900?'Peak: '+d.peak_temp+' °C':'';
    document.getElementById('pkw').textContent=d.peak_watt>0?'Peak: '+d.peak_watt+' W':'';
    if(atab===0&&_liveMode) loadHistory();
    // Sistemske info v Tab 3
    if(atab===3)buildSysInfo(d);
    // Boost status
    const bEl=document.getElementById('bStatus');
    if(bEl){
      if(d.boost_active){bEl.textContent='AKTIVEN +'+d.boost_pct+'%';bEl.style.color='#ff9500';}
      else{bEl.textContent='neaktiven';bEl.style.color='#555';}
    }
    if(d.boost_pct !== undefined){
      if(_curveData) _curveData.boostPct = d.boost_pct;
      const bPctEl=document.getElementById('bPct');
      if(bPctEl && bPctEl.readOnly) bPctEl.value=d.boost_pct;
    }
    if(atab===1 && _curveData){
      const liveTemp = parseFloat(document.getElementById('ct').textContent) || null;
      const boostStr = d.boost_active ? ('1'+d.boost_pct) : '0';
      const curKey   = (liveTemp ? liveTemp.toFixed(1) : 'null') + '|' + boostStr;
      if(curKey !== _lastLiveTemp){
        _lastLiveTemp = curKey;
        drawFanCurveFromCache();
      }
    }
  }catch(e){}
}
setInterval(refreshData,10000);
refreshData();

// ── uPlot ──────────────────────────────────────────────────────────
const COLORS={temp:'#e05252',hum:'#5b9bd5',fan:'#4ec9b0',watt:'#d4a76a',grid:'#2a2a2a',text:'#888'};
let _uplot=null,_rawData=null;
let _lastCurveFanPct=null,_lastLiveTemp=null;

// Graf state
let _periodSec  = 3600;
let _liveMode   = true;
let _viewCenter = 0;
let _graphH     = 320;
let _cache      = null;
let _fetchPending = false;

function setPeriod(sec){
  _periodSec = sec;
  document.querySelectorAll('.pbtn').forEach(b=>{
    b.classList.toggle('active', parseInt(b.dataset.sec)===sec);
  });
  _cache = null;
  if(_liveMode) loadHistory();
  else          applyViewWindow();
}

function gotoLive(){
  _liveMode = true;
  _cache    = null;
  document.getElementById('btnLive').style.background='#30d158';
  loadHistory();
}

function resetGraph(){
  _periodSec  = 3600;
  _liveMode   = true;
  _cache      = null;
  _graphH     = 320;
  document.getElementById('graphHeight').value = 320;
  document.getElementById('graphHeightVal').textContent = '320px';
  document.querySelectorAll('.pbtn').forEach(b=>{
    b.classList.toggle('active', parseInt(b.dataset.sec)===3600);
  });
  document.getElementById('btnLive').style.background='#30d158';
  loadHistory();
}

function onGraphHeightChange(){
  const v=parseInt(document.getElementById('graphHeight').value);
  _graphH=v;
  document.getElementById('graphHeightVal').textContent=v+'px';
  if(_uplot) _uplot.setSize({width:getPlotWidth(),height:v});
}

function buildSeries(){
  const s=[{}];
  if(document.getElementById('cbTemp').checked)
    s.push({label:'Temp °C',stroke:COLORS.temp,width:1.5,fill:COLORS.temp+'18',
            scale:'temp',value:(u,v)=>v==null?'--':v.toFixed(1)+' °C'});
  if(document.getElementById('cbHum').checked)
    s.push({label:'Vlaga %',stroke:COLORS.hum,width:1.5,scale:'hum',
            value:(u,v)=>v==null?'--':v.toFixed(0)+' %'});
  if(document.getElementById('cbFan').checked)
    s.push({label:'Fan %',stroke:COLORS.fan,width:1.5,scale:'fan',
            value:(u,v)=>v==null?'--':v.toFixed(0)+' %'});
  if(document.getElementById('cbWatt').checked)
    s.push({label:'Watt',stroke:COLORS.watt,width:1.5,scale:'watt',
            value:(u,v)=>v==null?'--':v.toFixed(1)+' W'});
  return s;
}

function buildAxes(){
  const cbT=document.getElementById('cbTemp').checked,
        cbH=document.getElementById('cbHum').checked,
        cbF=document.getElementById('cbFan').checked,
        cbW=document.getElementById('cbWatt').checked;
  const ax={stroke:COLORS.text,grid:{stroke:COLORS.grid,width:0.5},
            ticks:{show:false},font:'10px monospace'};
  const axes=[{...ax,values:(u,vs)=>vs.map(v=>{
    if(v==null)return'';
    const d=new Date(v*1000);
    const h=d.getHours().toString().padStart(2,'0');
    const m=d.getMinutes().toString().padStart(2,'0');
    return h+':'+m;
  })}];
  let ld=false;
  if(cbT){axes.push({...ax,scale:'temp',stroke:COLORS.temp,label:'°C',size:36,side:ld?1:3});if(!ld)ld=true;}
  if(cbH){axes.push({...ax,scale:'hum',stroke:COLORS.hum,label:'%',size:36,side:ld?1:3,grid:{show:false}});if(!ld)ld=true;}
  if(cbF){axes.push({...ax,scale:'fan',stroke:COLORS.fan,label:'Fan%',size:36,side:1,grid:{show:false}});}
  if(cbW){axes.push({...ax,scale:'watt',stroke:COLORS.watt,label:'W',size:36,side:1,grid:{show:false}});}
  return axes;
}

function buildScales(){
  return{
    x:{time:true},
    temp:{range:(u,mn,mx)=>[mn-1,mx+1]},
    hum:{range:(u,mn,mx)=>[Math.max(0,mn-3),Math.min(100,mx+3)]},
    fan:{range:(u,mn,mx)=>[Math.max(0,mn-3),Math.min(100,mx+3)]},
    watt:{range:(u,mn,mx)=>[Math.max(0,mn-0.5),mx+0.5]}
  };
}

function buildData(raw){
  if(!raw)return[[]];
  const cbT=document.getElementById('cbTemp').checked,
        cbH=document.getElementById('cbHum').checked,
        cbF=document.getElementById('cbFan').checked,
        cbW=document.getElementById('cbWatt').checked;
  const o=[raw[0]];
  if(cbT)o.push(raw[1]);if(cbH)o.push(raw[2]);if(cbF)o.push(raw[3]);if(cbW)o.push(raw[4]);
  return o;
}

function getPlotWidth(){
  const w=document.getElementById('uplotWrap');
  return w?Math.max(300,w.clientWidth):600;
}

function getMaxPts(){
  const w=document.getElementById('uplotWrap');
  return w?Math.max(400,Math.min(1200,w.clientWidth)):800;
}

function onPlotWheel(e){
  e.preventDefault();
  if(!_uplot||!_rawData||_rawData[0].length===0) return;

  const xs   = _rawData[0];
  const xMin = xs[0];
  const xMax = xs[xs.length-1];

  if(e.ctrlKey){
    const periods=[600,3600,21600,86400,259200,604800,0];
    let idx=periods.indexOf(_periodSec);
    if(idx<0) idx=1;
    if(e.deltaY>0) idx=Math.min(idx+1,periods.length-1);
    else           idx=Math.max(idx-1,0);
    setPeriod(periods[idx]);
  } else {
    const winSec = _periodSec>0 ? _periodSec : (xMax-xMin);
    const step   = winSec * 0.20;
    const dir    = e.deltaY>0 ? 1 : -1;

    let curMin = _uplot.scales.x.min;
    let curMax = _uplot.scales.x.max;
    let newMin = curMin + dir*step;
    let newMax = curMax + dir*step;

    if(newMax > xMax){
      newMax = xMax;
      newMin = newMax - (curMax-curMin);
      _liveMode = true;
      document.getElementById('btnLive').style.background='#30d158';
    } else {
      _liveMode = false;
      document.getElementById('btnLive').style.background='#555';
    }

    if(newMin < xMin){ newMin=xMin; newMax=Math.min(xMin+(curMax-curMin),xMax); }

    _viewCenter = (newMin+newMax)/2;
    _uplot.setScale('x',{min:newMin,max:newMax});
    checkPrefetch(newMin, newMax);
  }
}

async function checkPrefetch(visMin, visMax){
  if(_fetchPending) return;
  if(!_cache)       return;

  const winSec = visMax - visMin;
  const margin = winSec;

  const cacheLeft  = visMin - _cache.fromTs;
  const cacheRight = _cache.toTs - visMax;

  if(cacheLeft < margin*0.3 || cacheRight < margin*0.3){
    const fetchFrom = Math.max(0, Math.floor(visMin - margin));
    const fetchTo   = Math.floor(visMax + margin);
    await fetchAndMerge(fetchFrom, fetchTo);
  }
}

async function fetchAndMerge(fromTs, toTs){
  if(_fetchPending) return;
  _fetchPending=true;
  try{
    const url = '/api/history?from='+fromTs+'&to='+toTs+'&maxPts='+getMaxPts();
    const pts = await(await fetch(url)).json();
    if(!pts||pts.length===0){ _fetchPending=false; return; }

    const n=pts.length;
    const xs=new Float64Array(n),temp=new Float64Array(n),
          hum=new Float64Array(n),fan=new Float64Array(n),watt=new Float64Array(n);
    for(let i=0;i<n;i++){
      xs[i]=pts[i].ts; temp[i]=pts[i].temp; hum[i]=pts[i].hum;
      fan[i]=pts[i].fan; watt[i]=pts[i].watt;
    }
    const newRaw=[xs,temp,hum,fan,watt];

    const curMin=_uplot?_uplot.scales.x.min:null;
    const curMax=_uplot?_uplot.scales.x.max:null;

    _rawData=newRaw;
    _cache={fromTs, toTs, raw:newRaw};

    if(!_uplot){ initUplot(newRaw); }
    else{
      _uplot.setData(buildData(newRaw), false);
      if(curMin&&curMax&&!_liveMode){
        _uplot.setScale('x',{min:curMin,max:curMax});
      }
    }
  }catch(e){}
  _fetchPending=false;
}

function initUplot(raw){
  _rawData=raw;
  const wrap=document.getElementById('uplotWrap');
  if(!wrap)return;
  wrap.innerHTML='';

  const has=['cbTemp','cbHum','cbFan','cbWatt'].some(id=>document.getElementById(id).checked);
  if(!has){
    wrap.innerHTML='<div style="text-align:center;color:#555;padding:40px;font-size:12px">Izberi vsaj en podatek</div>';
    return;
  }

  _uplot=new uPlot({
    width:getPlotWidth(),
    height:_graphH,
    cursor:{sync:{key:'main'},drag:{x:true,y:false,setScale:false}},
    select:{show:true},
    legend:{show:true,live:true,markers:{show:true}},
    axes:buildAxes(),
    scales:buildScales(),
    series:buildSeries(),
    hooks:{
      setSelect:[u=>{
        const sel=u.select;
        if(sel.width>10){
          const xMin=u.posToVal(sel.left,'x');
          const xMax=u.posToVal(sel.left+sel.width,'x');
          u.setScale('x',{min:xMin,max:xMax});
          u.setSelect({left:0,top:0,width:0,height:0},false);
          _liveMode=false;
          document.getElementById('btnLive').style.background='#555';
          _periodSec=-1;
          document.querySelectorAll('.pbtn').forEach(b=>b.classList.remove('active'));
        }
      }],
      dblclick:[u=>{ gotoLive(); }]
    }
  }, buildData(raw), wrap);

  const canvas=wrap.querySelector('canvas');
  if(canvas) canvas.addEventListener('wheel', onPlotWheel, {passive:false});

  applyViewWindow();
}

function applyViewWindow(){
  if(!_uplot||!_rawData||_rawData[0].length===0) return;
  const xs  = _rawData[0];
  const xMax= xs[xs.length-1];
  const xMin= xs[0];

  if(_periodSec===0||_periodSec<0){
    _uplot.setScale('x',{min:xMin,max:xMax});
    return;
  }

  if(_liveMode){
    _uplot.setScale('x',{min:xMax-_periodSec, max:xMax});
  } else {
    const half=_periodSec/2;
    let wMin=_viewCenter-half;
    let wMax=_viewCenter+half;
    if(wMax>xMax){wMax=xMax;wMin=wMax-_periodSec;}
    if(wMin<xMin){wMin=xMin;wMax=Math.min(xMin+_periodSec,xMax);}
    _uplot.setScale('x',{min:wMin,max:wMax});
  }
}

function rebuildUplot(){ if(_rawData) initUplot(_rawData); }
['cbTemp','cbHum','cbFan','cbWatt'].forEach(id=>{
  const el=document.getElementById(id);
  if(el) el.addEventListener('change',rebuildUplot);
});

let _rt=null;
window.addEventListener('resize',()=>{
  clearTimeout(_rt);
  _rt=setTimeout(()=>{
    if(_uplot) _uplot.setSize({width:getPlotWidth(),height:_graphH});
  },150);
});

async function loadHistory(){
  if(_fetchPending) return;

  const nowTs = Math.floor(Date.now()/1000);

  let fetchFrom, fetchTo;

  if(_periodSec===0){
    fetchFrom=0;
    fetchTo=nowTs;
  } else if(_periodSec>86400){
    fetchFrom=0;
    fetchTo=nowTs;
  } else {
    const winSec=_periodSec;
    if(_liveMode){
      fetchTo   = nowTs;
      fetchFrom = nowTs - winSec*3;
    } else {
      const half=winSec/2;
      fetchFrom = Math.floor(_viewCenter - half - winSec);
      fetchTo   = Math.floor(_viewCenter + half + winSec);
      fetchTo   = Math.min(fetchTo, nowTs);
    }
    fetchFrom = Math.max(0, fetchFrom);
  }

  await fetchAndMerge(fetchFrom, fetchTo);
  applyViewWindow();
}

// ── Monitor ────────────────────────────────────────────────────────
async function refreshMonitor(){
  try{
    const d=await(await fetch('/api/monitor')).json();
    document.getElementById('mpwr').textContent=d.powered?'ON':'OFF';
    document.getElementById('mpwr').style.color=d.powered?'#30d158':'#ff9500';
    document.getElementById('msvc').textContent=d.port_ok+'/'+d.port_total;
    document.getElementById('msvd').textContent=d.all_ok?'VSI OK':'NAPAKA';
    const box=document.getElementById('monBox');box.style.display='';
    const t=document.getElementById('monTbl');
    t.innerHTML='<tr><th>Port</th><th>Servis</th><th>Status</th></tr>';
    d.ports.forEach(p=>{if(!p.enabled)return;const tr=document.createElement('tr');tr.innerHTML=`<td>${p.port}</td><td>${p.name}</td><td>${p.ok?'&#10003;':'&#10007;'}</td>`;t.appendChild(tr);});
  }catch(e){}
}
setInterval(refreshMonitor,300000);refreshMonitor();

// ══════════════════════════════════════════════════════════════════
// FAN CURVE SVG GRAF
// ══════════════════════════════════════════════════════════════════

const CURVE_DEF_TEMP = [30, 38, 45, 52, 58, 65];
const CURVE_DEF_PCT  = [0, 0, 3, 20, 50, 100];
const CURVE_LABELS   = ['Mirovanje','Lahka obremenitev','Zmerna obremenitev',
                        'Višja obremenitev','Visoka obremenitev','Maksimum'];

const GC = {
  left:48, right:16, top:16, bottom:36,
  w: 520, h: 220,
  tMin:25, tMax:70,
  pMin:0,  pMax:100,
};
GC.plotW = GC.w - GC.left - GC.right;
GC.plotH = GC.h - GC.top  - GC.bottom;

function gcX(temp){ return GC.left + (temp - GC.tMin)/(GC.tMax - GC.tMin)*GC.plotW; }
function gcY(pct) { return GC.top  + (1 - pct/GC.pMax)*GC.plotH; }

function svgEl(tag, attrs){
  const el=document.createElementNS('http://www.w3.org/2000/svg',tag);
  for(const[k,v] of Object.entries(attrs)) el.setAttribute(k,v);
  return el;
}

function confColor(conf, thresh){
  if(conf >= thresh)   return '#30d158';
  if(conf >= thresh/2) return '#ffd700';
  return '#555';
}

let _curveData = null;
let _editIdx   = -1;

function drawFanCurve(s, liveTemp, boostActive, boostPct){
  const svg = document.getElementById('fanCurveSvg');
  if(!svg) return;
  svg.innerHTML = '';

  const thresh = s.adaptThresh || 20;

  [0,25,50,75,100].forEach(p=>{
    const y=gcY(p);
    svg.appendChild(svgEl('line',{x1:GC.left,y1:y,x2:GC.left+GC.plotW,y2:y,
      stroke:'#1e1e1e','stroke-width':'1'}));
    svg.appendChild(svgEl('text',{x:GC.left-4,y:y+4,'text-anchor':'end',
      fill:'#444','font-size':'9','font-family':'monospace'})).textContent=p+'%';
  });
  s.curveTemp.forEach(t=>{
    const x=gcX(t);
    svg.appendChild(svgEl('line',{x1:x,y1:GC.top,x2:x,y2:GC.top+GC.plotH,
      stroke:'#1a1a1a','stroke-width':'1'}));
    svg.appendChild(svgEl('text',{x:x,y:GC.top+GC.plotH+14,'text-anchor':'middle',
      fill:'#444','font-size':'9','font-family':'monospace'})).textContent=t+'°';
  });

  svg.appendChild(svgEl('line',{x1:GC.left,y1:GC.top,x2:GC.left,y2:GC.top+GC.plotH,
    stroke:'#333','stroke-width':'1'}));
  svg.appendChild(svgEl('line',{x1:GC.left,y1:GC.top+GC.plotH,x2:GC.left+GC.plotW,y2:GC.top+GC.plotH,
    stroke:'#333','stroke-width':'1'}));

  let defPts = CURVE_DEF_TEMP.map((t,i)=>`${gcX(t)},${gcY(CURVE_DEF_PCT[i])}`).join(' ');
  svg.appendChild(svgEl('polyline',{points:defPts,fill:'none',stroke:'#444',
    'stroke-width':'1.5','stroke-dasharray':'5,4'}));

  let actPts = s.curveTemp.map((t,i)=>`${gcX(t)},${gcY(s.curvePct[i])}`).join(' ');
  const fillPts = `${gcX(s.curveTemp[0])},${gcY(0)} ` + actPts +
                  ` ${gcX(s.curveTemp[5])},${gcY(0)}`;
  svg.appendChild(svgEl('polygon',{points:fillPts,fill:'#00d4ff0d',stroke:'none'}));
  svg.appendChild(svgEl('polyline',{points:actPts,fill:'none',stroke:'#00d4ff',
    'stroke-width':'2'}));

  s.curveTemp.forEach((t,i)=>{
    const x = gcX(t);
    const y = gcY(s.curvePct[i]);
    const locked = s.curveLocked && s.curveLocked[i];
    const conf   = s.curveConfidence ? s.curveConfidence[i] : 0;
    const cColor = confColor(conf, thresh);

    const hit = svgEl('circle',{cx:x,cy:y,r:'22',fill:'transparent',
      style:'cursor:pointer'});
    hit.addEventListener('click', e=>{ e.stopPropagation(); editPopupOpen(i,x,y,s); });
    svg.appendChild(hit);

    svg.appendChild(svgEl('circle',{cx:x,cy:y,r:'7',fill:'none',
      stroke:cColor,'stroke-width':'2'}));

    svg.appendChild(svgEl('circle',{cx:x,cy:y,r:'4.5',
      fill: locked ? '#ff9500' : '#00d4ff'}));

    if(locked){
      const lk=svgEl('text',{x:x+8,y:y-6,fill:'#ff9500','font-size':'9',
        'font-family':'monospace'});
      lk.textContent='🔒';
      svg.appendChild(lk);
    }

    const vt=svgEl('text',{x:x,y:y-11,'text-anchor':'middle',fill:'#00d4ff',
      'font-size':'10','font-family':'monospace','font-weight':'bold'});
    vt.textContent=s.curvePct[i]+'%';
    svg.appendChild(vt);
  });

  if(liveTemp && liveTemp > GC.tMin && liveTemp < GC.tMax){
    const tx = gcX(liveTemp);
    svg.appendChild(svgEl('line',{x1:tx,y1:GC.top,x2:tx,y2:GC.top+GC.plotH,
      stroke:'#e05252','stroke-width':'1.5','stroke-dasharray':'3,2'}));
    let fPct = s.curvePct[0];
    for(let i=0;i<s.curveTemp.length-1;i++){
      if(liveTemp>=s.curveTemp[i]&&liveTemp<s.curveTemp[i+1]){
        const r=(liveTemp-s.curveTemp[i])/(s.curveTemp[i+1]-s.curveTemp[i]);
        fPct=s.curvePct[i]+r*(s.curvePct[i+1]-s.curvePct[i]);
      }
    }
    const ty = gcY(fPct);
    const tb=svgEl('rect',{x:tx-18,y:GC.top+GC.plotH+3,width:36,height:12,
      fill:'#e05252',rx:'3'});
    svg.appendChild(tb);
    const tt=svgEl('text',{x:tx,y:GC.top+GC.plotH+12,'text-anchor':'middle',
      fill:'#fff','font-size':'9','font-family':'monospace','font-weight':'bold'});
    tt.textContent=parseFloat(liveTemp).toFixed(1)+'°';
    svg.appendChild(tt);

    if(boostActive && boostPct>0){
      const byBase = gcY(fPct);
      const byTop  = gcY(Math.min(100, fPct + boostPct));
      svg.appendChild(svgEl('line',{x1:tx,y1:byBase,x2:tx,y2:byTop,
        stroke:'#ff9500','stroke-width':'2.5'}));
      svg.appendChild(svgEl('polygon',{
        points:`${tx},${byTop-1} ${tx-5},${byTop+7} ${tx+5},${byTop+7}`,
        fill:'#ff9500'}));
      const bt=svgEl('text',{x:tx+8,y:byTop+4,fill:'#ff9500',
        'font-size':'9','font-family':'monospace','font-weight':'bold'});
      bt.textContent='+'+boostPct+'%';
      svg.appendChild(bt);
    }
  }
}

function editPopupOpen(idx, svgX, svgY, s){
  _editIdx = idx;
  const popup  = document.getElementById('curveEditPopup');
  const sec    = document.getElementById('fanCurveSec');
  const svg    = document.getElementById('fanCurveSvg');
  const locked = s.curveLocked && s.curveLocked[idx];

  document.getElementById('editPopupLabel').textContent =
    'Točka '+idx+' — '+CURVE_LABELS[idx]+' ('+s.curveTemp[idx]+'°C)';
  document.getElementById('editPopupInput').value = s.curvePct[idx];
  document.getElementById('editPopupTemp').value  = s.curveTemp[idx];
  document.getElementById('editPopupErr').style.display = 'none';
  document.getElementById('editPopupLockWarn').textContent =
    locked ? '⚠ Točka zaklenjena — algoritem ne bo posodabljal' : '';

  const svgRect = svg.getBoundingClientRect();
  const secRect = sec.getBoundingClientRect();
  const scaleX  = svgRect.width  / 520;
  const scaleY  = svgRect.height / 220;
  let px = svgRect.left - secRect.left + svgX * scaleX;
  let py = svgRect.top  - secRect.top  + svgY * scaleY - 60;

  px = Math.max(4, Math.min(px, secRect.width - 180));
  py = Math.max(4, py);

  popup.style.left = px+'px';
  popup.style.top  = py+'px';
  popup.style.display = 'block';

  setTimeout(()=>document.getElementById('editPopupInput').select(), 50);
}

function editPopupClose(){
  document.getElementById('curveEditPopup').style.display='none';
  _editIdx = -1;
}

function editPopupApply(){
  if(_editIdx < 0) return;
  const pct  = parseInt(document.getElementById('editPopupInput').value);
  const temp = parseFloat(document.getElementById('editPopupTemp').value);
  const errEl = document.getElementById('editPopupErr');

  if(isNaN(pct)||pct<0||pct>100||isNaN(temp)||temp<0||temp>80){
    errEl.textContent='Neveljavna vrednost!';errEl.style.display='';return;
  }
  if(_curveData){
    const temps = [..._curveData.curveTemp];
    temps[_editIdx] = temp;
    for(let i=0;i<temps.length-1;i++){
      if(temps[i]+0.5>temps[i+1]){
        errEl.textContent='Temperatura mora biti vsaj 0.5° nižja od naslednje točke!';
        errEl.style.display='';return;
      }
    }
  }

  const inpPct  = document.getElementById('cp'+_editIdx);
  const inpTemp = document.getElementById('ct'+_editIdx);
  if(inpPct)  inpPct.value  = pct;
  if(inpTemp) inpTemp.value = temp;

  const savedIdx = _editIdx;
  editPopupClose();
  saveFan('msgF1').then(()=>{
    if(_curveData){
      _curveData.curvePct[savedIdx]  = pct;
      _curveData.curveTemp[savedIdx] = temp;
      drawFanCurveFromCache();
    }
  });
}

document.addEventListener('keydown', e=>{
  if(document.getElementById('curveEditPopup').style.display==='block'){
    if(e.key==='Enter')  editPopupApply();
    if(e.key==='Escape') editPopupClose();
  }
});
document.addEventListener('click', e=>{
  const popup=document.getElementById('curveEditPopup');
  if(popup.style.display==='block' && !popup.contains(e.target) &&
     !e.target.closest('#fanCurveSvg')) editPopupClose();
});

function drawFanCurveFromCache(){
  if(!_curveData) return;
  const liveTemp   = parseFloat(document.getElementById('ct').textContent) || null;
  const bEl        = document.getElementById('bStatus');
  const boostActive= bEl && bEl.textContent.includes('AKTIVEN');
  const boostPct   = _curveData.boostPct || 0;
  drawFanCurve(_curveData, liveTemp, boostActive, boostPct);
}

// ── Fan nastavitve ─────────────────────────────────────────────────
function onBoostLockChange(){
  const locked = document.getElementById('bLock').checked;
  const inp    = document.getElementById('bPct');
  const badge  = document.getElementById('bPctConf');
  if(locked){
    inp.readOnly = false;
    inp.style.borderColor = '#333';
    inp.style.color = '#e0e0e0';
    badge.textContent = 'LOCKED';
    badge.style.background = '#ff950033';
    badge.style.color = '#ff9500';
  } else {
    inp.readOnly = true;
    inp.style.borderColor = '#ff950055';
    inp.style.color = '#888';
    badge.textContent = 'AUTO';
    badge.style.background = '#00d4ff22';
    badge.style.color = '#00d4ff';
  }
}
async function loadFan(){
  try{
    const s=await(await fetch('/api/fansettings')).json();
    for(let i=0;i<6;i++){
      document.getElementById('ct'+i).value=s.curveTemp[i];
      document.getElementById('cp'+i).value=s.curvePct[i];
      if(s.curveLocked)document.getElementById('cl'+i).checked=s.curveLocked[i];
      if(s.curveConfidence){
        const conf=s.curveConfidence[i];
        const thresh=s.adaptThresh||20;
        const el=document.getElementById('cc'+i);
        el.textContent=conf+'/'+thresh;
        el.style.color=conf>=thresh?'#30d158':'#555';
      }
    }
    document.getElementById('fanStartPct').value=s.fanStartPct;
    document.getElementById('fanStopPct').value=s.fanStopPct;
    document.getElementById('fMaxD').value=s.fanMaxDayPct;
    document.getElementById('fDndM').value=s.dndMaxPct;
    document.getElementById('dndE').checked=s.dndEnabled;
    document.getElementById('dndF').value=s.dndFrom;
    document.getElementById('dndT').value=s.dndTo;
    try{const m=await(await fetch('/api/fan/manual')).json();document.getElementById('manToggle').checked=m.manual;document.getElementById('manPct').value=m.pct;document.getElementById('manPctVal').textContent=m.pct+'%';updateManUI(m.manual,m.pct);}catch(e){}
    if(s.boostWattThreshold!==undefined){
      document.getElementById('bWth').value=s.boostWattThreshold;
      document.getElementById('bPct').value=s.boostPct;
      document.getElementById('bEval').value=s.boostEvalSec||20;document.getElementById('bLearn').value=s.boostLearnSec||120;
      document.getElementById('bLock').checked=s.boostLocked||false;
      onBoostLockChange();
    }
    _curveData = s;
    _lastCurveFanPct = JSON.stringify(s.curvePct);
    _lastLiveTemp = null;
    drawFanCurveFromCache();
    _fL=true;
  }catch(e){}
}
async function saveFan(msgId='msgF5'){
  const ct=[0,1,2,3,4,5].map(i=>parseFloat(document.getElementById('ct'+i).value)||0);
  const cp=[0,1,2,3,4,5].map(i=>parseInt(document.getElementById('cp'+i).value)||0);
  const cl=[0,1,2,3,4,5].map(i=>document.getElementById('cl'+i).checked);

  for(let i=0;i<ct.length-1;i++){
    if(ct[i]+0.5>ct[i+1]){
      const m=document.getElementById(msgId);
      m.textContent='Napaka: točka '+(i+1)+' mora biti vsaj 0.5° višja od točke '+i+'!';
      m.style.color='#ff3b30';
      setTimeout(()=>m.textContent='',5000);
      return Promise.resolve();
    }
  }

  const b={
    curveTemp:ct,curvePct:cp,curveLocked:cl,
    fanStartPct:parseInt(document.getElementById('fanStartPct').value)||33,
    fanStopPct:parseInt(document.getElementById('fanStopPct').value)||27,
    fanMaxDayPct:parseInt(document.getElementById('fMaxD').value)||100,
    dndMaxPct:parseInt(document.getElementById('fDndM').value)||0,
    dndEnabled:document.getElementById('dndE').checked,
    dndFrom:parseInt(document.getElementById('dndF').value)||22,
    dndTo:parseInt(document.getElementById('dndT').value)||7,
    boostWattThreshold:parseFloat(document.getElementById('bWth').value)||10,
    boostPct:parseInt(document.getElementById('bPct').value)||20,
    boostLocked:document.getElementById('bLock').checked,
    boostEvalSec:parseInt(document.getElementById('bEval').value)||20,boostLearnSec:parseInt(document.getElementById('bLearn').value)||120
  };
  const dndMax = parseInt(document.getElementById('fDndM').value) || 0;
  const fanStartPwm = parseInt(document.getElementById('fanStartPct').value) || 33;
  // fanStartPct je %PWM — pretvorba v %user (identična _pwmToUser v fan.cpp)
  const fanStartUser = fanStartPwm <= 27 ? 1 : Math.round(1 + (fanStartPwm - 27) * 99 / 73);
  if (dndMax > 0 && dndMax < fanStartUser) {
    const m = document.getElementById(msgId);
    m.textContent = 'DND max mora biti 0% ali vsaj ' + fanStartUser + '%!';
    m.style.color = '#ff3b30';
    setTimeout(() => m.textContent = '', 5000);
    return Promise.resolve();
  }
  const r=await fetch('/save/fan',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById(msgId);
  m.textContent=r.ok?'Shranjeno!':'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
  if(r.ok) loadFan();
  return r;
}
async function adaptReset(){
  if(!confirm('Ponastavi vse naučene vrednosti na default? Zaklepi ostanejo.'))return;
  const r=await fetch('/adapt/reset',{method:'POST'});
  const m=document.getElementById('msgF3');
  m.textContent=r.ok?'Ucenje ponastavljeno — default vrednosti aktivne':'Napaka!';
  m.style.color=r.ok?'#ff9500':'#ff3b30';
  setTimeout(()=>{m.textContent='';loadFan();},3000);
}

// ── Kalibracija ────────────────────────────────────────────────────
async function loadCal(){
  try{
    const s=await(await fetch('/api/calsettings')).json();
    document.getElementById('tOff').value=s.tempOffset;
    document.getElementById('hOff').value=s.humOffset;
    document.getElementById('sOhm').value=s.shuntOhms;
    document.getElementById('cCorr').value=s.currentCorr;
    document.getElementById('pwmFreq').value=s.fanPwmFreq||25000;
    updatePwmInvUI(s.fanPwmInvert||false);
    _cL=true;
  }catch(e){}
}
async function saveCal(){
  const b={tempOffset:parseFloat(document.getElementById('tOff').value)||0,humOffset:parseFloat(document.getElementById('hOff').value)||0,shuntOhms:parseFloat(document.getElementById('sOhm').value)||0.1,currentCorr:parseFloat(document.getElementById('cCorr').value)||1};
  const r=await fetch('/save/cal',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgC');m.textContent=r.ok?'Shranjeno!':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';setTimeout(()=>m.textContent='',5000);
}

// ── Restart ────────────────────────────────────────────────────────
async function doRestart(){
  if(!confirm('Ponovni zagon naprave? Stran se bo osvežila čez 6 sekund.'))return;
  const m=document.getElementById('msgRestart');
  try{
    await fetch('/api/restart',{method:'POST'});
    m.textContent='Naprava se restartira...';m.style.color='#ff9500';
    setTimeout(()=>location.reload(),6000);
  }catch(e){m.textContent='Napaka!';m.style.color='#ff3b30';}
}
async function doSettingsReset(){
  if(!confirm('POZOR: Vse nastavitve (krivulja, kalibracija, DND, Boost...) bodo ponastavljene na privzete vrednosti!\nWiFi nastavitve ostanejo.\n\nNadaljujem?'))return;
  const m=document.getElementById('msgSettingsReset');
  try{
    const r=await fetch('/api/settings/reset',{method:'POST'});
    if(r.ok){
      m.textContent='Ponastavljeno! Naprava se restartira...';m.style.color='#ff9500';
      setTimeout(()=>location.reload(),6000);
    }else{m.textContent='Napaka!';m.style.color='#ff3b30';}
  }catch(e){m.textContent='Napaka!';m.style.color='#ff3b30';}
}

// ── WiFi Tab ───────────────────────────────────────────────────────
let _wifiSlots=[];

async function loadWifi(){
  await Promise.all([loadWifiStatus(),loadWifiHistory(),loadWifiNetworks()]);
  _wL=true;
}

async function loadWifiStatus(){
  try{
    const d=await(await fetch('/api/wifi/status')).json();
    document.getElementById('wCurrentSsid').textContent=d.connected?d.ssid:'--';
    document.getElementById('wCurrentBssid').textContent=d.connected?d.bssid:'--';
    document.getElementById('wCurrentCh').textContent=d.connected?d.channel:'--';
    document.getElementById('wCurrentRssi').textContent=d.connected?(d.rssi+' dBm'):'--';
    const sc=!d.last_scan_s?'nikoli':d.last_scan_s<60?(d.last_scan_s+'s nazaj'):(Math.round(d.last_scan_s/60)+' min nazaj');
    document.getElementById('wLastScan').textContent=sc;
    if(d.aps&&d.aps.length>0)renderWifiBarChart(d.aps);
  }catch(e){}
}

async function loadWifiHistory(){
  try{
    const arr=await(await fetch('/api/wifi/history')).json();
    const t=document.getElementById('wifiHistTbl');
    t.innerHTML='<tr><th>Čas</th><th>Event</th><th>SSID</th><th>BSSID</th><th>RSSI pred</th><th>RSSI po</th></tr>';
    const types=['SCAN','RECONNECT','MANUAL','BOOT'];
    arr.forEach(e=>{
      const tr=document.createElement('tr');
      const ts=e.ts>1000000?new Date(e.ts*1000).toLocaleTimeString('sl-SI'):e.ts+'s';
      tr.innerHTML=`<td>${ts}</td><td>${types[e.type]||e.type}</td><td>${e.ssid}</td><td style="font-size:10px">${e.bssid}</td><td>${e.rssi_pre} dBm</td><td style="color:#30d158">${e.rssi_post} dBm</td>`;
      t.appendChild(tr);
    });
  }catch(e){}
}

async function loadWifiNetworks(){
  try{
    const d=await(await fetch('/api/wifi/networks')).json();
    renderWifiSlots(d.slots);
  }catch(e){}
}

function renderWifiBarChart(aps){
  aps.sort((a,b)=>b.rssi-a.rssi);
  const wrap=document.getElementById('wifiBarChart');
  if(!wrap)return;
  const W=Math.max(400,wrap.clientWidth);
  const rowH=40,padL=160,padR=90,padT=8,padB=8;
  const H=padT+aps.length*rowH+padB;
  const rssiMin=-95,rssiMax=-40;
  const barW=W-padL-padR;
  function rssiColor(r){return r>=-65?'#30d158':r>=-80?'#ff9500':'#ff3b30';}
  function rssiPct(r){return Math.max(0,Math.min(1,(r-rssiMin)/(rssiMax-rssiMin)));}
  // Znani SSID-ji iz slotov (imajo geslo)
  const knownSsids=new Set(_wifiSlots.filter(s=>s.enabled&&s.ssid).map(s=>s.ssid));
  let svg=`<svg viewBox="0 0 ${W} ${H}" style="width:100%;display:block">`;
  aps.forEach((ap,i)=>{
    const y=padT+i*rowH;
    const bw=rssiPct(ap.rssi)*barW;
    const col=rssiColor(ap.rssi);
    const con=ap.isConnected;
    const known=knownSsids.has(ap.ssid)&&!con;
    if(con) svg+=`<rect x="0" y="${y+1}" width="${W}" height="${rowH-2}" fill="#00d4ff0a" rx="3"/>`;
    else if(known) svg+=`<rect x="0" y="${y+1}" width="${W}" height="${rowH-2}" fill="#30d1580a" rx="3"/>`;
    if(known) svg+=`<rect x="0" y="${y+4}" width="3" height="${rowH-8}" fill="#30d158" rx="1"/>`;
    const lbl=(ap.ssid||'(skrit)').substring(0,18);
    svg+=`<text x="8" y="${y+14}" text-anchor="start" fill="${con?'#00d4ff':known?col:'#555'}" font-size="11" font-family="monospace" font-weight="${con||known?'bold':'normal'}">${lbl}</text>`;
    svg+=`<text x="8" y="${y+27}" text-anchor="start" fill="#3a3a3a" font-size="9" font-family="monospace">${ap.bssid}</text>`;
    svg+=`<rect x="${padL}" y="${y+10}" width="${Math.max(2,bw)}" height="${rowH-20}" fill="${col}" rx="2" opacity="${con?'1':'0.7'}"/>`;
    svg+=`<text x="${padL+bw+6}" y="${y+rowH/2+4}" fill="${col}" font-size="10" font-family="monospace">${ap.rssi} dBm</text>`;
    svg+=`<text x="${W-4}" y="${y+14}" text-anchor="end" fill="#555" font-size="9" font-family="monospace">ch${ap.channel}</text>`;
    if(con) svg+=`<text x="${padL-6}" y="${y+14}" text-anchor="end" fill="#00d4ff" font-size="10">&#9679;</text>`;
    // → pomeni: znan SSID, dvoklikni za povezavo
    if(known) svg+=`<text x="${padL-6}" y="${y+14}" text-anchor="end" fill="${col}" font-size="11">&#8594;</text>`;
    // Overlay ZADNJI — data-connect (ne ondblclick!) ker inline SVG handlerji ne delujejo z innerHTML
    if(known){
      const ssidSafe=ap.ssid.replace(/&/g,'&amp;').replace(/"/g,'&quot;');
      svg+=`<rect x="0" y="${y}" width="${W}" height="${rowH}" fill="transparent" pointer-events="all" style="cursor:pointer" data-connect="${ssidSafe}"/>`;
    }
  });
  svg+='</svg>';
  wrap.innerHTML=svg;
  // addEventListener po innerHTML — edina zanesljiva metoda za SVG v HTML kontekstu
  wrap.querySelectorAll('[data-connect]').forEach(el=>{
    el.addEventListener('dblclick',()=>wifiConnectTo(el.getAttribute('data-connect')));
  });
}

async function wifiConnectTo(ssid){
  if(!confirm(`Poveži na "${ssid}"?\n(~12s prekinitve, ob neuspehu se samodejno vrne)`))return;
  const wrap=document.getElementById('wifiBarChart');
  const orig=wrap?wrap.innerHTML:'';
  if(wrap)wrap.innerHTML='<div style="padding:12px;color:#ff9500;font-family:monospace;font-size:11px">Vzpostavljam povezavo...</div>';
  try{
    const r=await fetch('/api/wifi/connect?ssid='+encodeURIComponent(ssid),{method:'POST'});
    if(!r.ok){if(wrap)wrap.innerHTML=orig;return;}
    // Poll dokler connect ne konča (max 18s)
    for(let i=0;i<36;i++){
      await new Promise(r=>setTimeout(r,500));
      try{
        const d=await(await fetch('/api/wifi/status')).json();
        if(!d.scan_in_progress){await loadWifi();return;}
      }catch(_){}
    }
  }catch(e){
    // Prekinitev med connectom — čakamo 10s in osvežimo
    await new Promise(r=>setTimeout(r,10000));
    try{await loadWifi();}catch(_){}
  }
}

function renderWifiSlots(slots){
  _wifiSlots=slots.map(s=>({...s}));
  const wrap=document.getElementById('wifiSlotsList');
  if(!wrap)return;
  wrap.innerHTML='';
  slots.forEach((slot,i)=>{
    const div=document.createElement('div');
    div.style='background:#161616;border:1px solid #2a2a2a;border-radius:6px;padding:10px 12px;margin-bottom:8px';
    div.innerHTML=`
    <div style="display:flex;align-items:center;gap:8px;margin-bottom:6px">
      <span style="font-size:10px;color:#555;min-width:50px">Slot ${i+1}</span>
      ${slot.isPreset?'<span style="font-size:9px;background:#00d4ff22;color:#00d4ff;padding:1px 6px;border-radius:3px">PRESET</span>':''}
      <label style="margin-left:auto;display:flex;align-items:center;gap:6px;font-size:11px;color:#aaa">
        <input type="checkbox" data-slot="${i}" class="wsEn"${slot.enabled?' checked':''} style="accent-color:#00d4ff"> Aktivno
      </label>
    </div>
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <div style="flex:1;min-width:120px">
        <div style="font-size:9px;color:#555;margin-bottom:2px">SSID</div>
        <input type="text" data-slot="${i}" class="wsSsid" value="${slot.ssid||''}" maxlength="31"
          style="width:100%;background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:4px 8px;border-radius:4px;font-family:monospace;font-size:12px">
      </div>
      <div style="flex:1;min-width:120px">
        <div style="font-size:9px;color:#555;margin-bottom:2px">Geslo${slot.isPreset&&slot.hasPass?' (nastavljeno)':''}</div>
        <input type="password" data-slot="${i}" class="wsPass"
          placeholder="${slot.isPreset&&slot.hasPass?'••••••••':'vnesi geslo'}" maxlength="63"
          style="width:100%;background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:4px 8px;border-radius:4px;font-family:monospace;font-size:12px">
      </div>
      <div style="display:flex;gap:4px;align-items:flex-end">
        <button onclick="wifiSlotMoveUp(${i})"${i===0?' disabled':''} class="btn bsm" style="padding:4px 8px;font-size:11px">&#8593;</button>
        <button onclick="wifiSlotMoveDown(${i})"${i===slots.length-1?' disabled':''} class="btn bsm" style="padding:4px 8px;font-size:11px">&#8595;</button>
        <button onclick="wifiSlotClear(${i})" class="btn bsm" style="padding:4px 8px;font-size:11px;background:#2a2a2a;color:#ff3b30">&#10005;</button>
      </div>
    </div>`;
    wrap.appendChild(div);
  });
}

function wifiSlotMoveUp(i){
  if(i===0)return;
  [_wifiSlots[i-1],_wifiSlots[i]]=[_wifiSlots[i],_wifiSlots[i-1]];
  renderWifiSlots(_wifiSlots);
}
function wifiSlotMoveDown(i){
  if(i>=_wifiSlots.length-1)return;
  [_wifiSlots[i],_wifiSlots[i+1]]=[_wifiSlots[i+1],_wifiSlots[i]];
  renderWifiSlots(_wifiSlots);
}
function wifiSlotClear(i){
  const activeCount=_wifiSlots.filter((s,idx)=>idx!==i&&s.enabled&&s.ssid).length;
  if(activeCount===0){alert('Vsaj eno omrežje mora biti aktivno!');return;}
  if(!confirm('Zbriši omrežje '+(_wifiSlots[i].ssid||'slot '+(i+1))+'?'))return;
  _wifiSlots[i]={ssid:'',pass:'',enabled:false,isPreset:false,hasPass:false};
  renderWifiSlots(_wifiSlots);
}

async function saveWifiSlots(){
  const slots=_wifiSlots.map((slot,i)=>{
    const ssidEl=document.querySelector(`.wsSsid[data-slot="${i}"]`);
    const passEl=document.querySelector(`.wsPass[data-slot="${i}"]`);
    const enEl=document.querySelector(`.wsEn[data-slot="${i}"]`);
    return{ssid:ssidEl?ssidEl.value.trim():slot.ssid,pass:passEl?passEl.value:'',enabled:enEl?enEl.checked:slot.enabled,isPreset:slot.isPreset};
  });
  const valid=slots.some(s=>s.enabled&&s.ssid.length>0);
  if(!valid){
    const m=document.getElementById('msgWifi');
    m.textContent='Vsaj eno omrežje mora biti aktivno!';m.style.color='#ff3b30';
    setTimeout(()=>m.textContent='',4000);return;
  }
  const r=await fetch('/api/wifi/slots',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({slots})});
  const m=document.getElementById('msgWifi');
  m.textContent=r.ok?'Shranjeno! Velja ob naslednjem WiFi reconnectu.':'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',4000);
}

async function wifiScanNow(btn){
  const orig=btn?btn.textContent:'';
  if(btn){btn.textContent='Skeniranje...';btn.disabled=true;}
  try{
    await fetch('/api/wifi/scan',{method:'POST'});
    // Poll dokler scan ne konča (max 15s — disconnect+scan+reconnect ~6-7s)
    for(let i=0;i<30;i++){
      await new Promise(r=>setTimeout(r,500));
      try{
        const d=await(await fetch('/api/wifi/status')).json();
        if(!d.scan_in_progress){await loadWifiStatus();break;}
      }catch(_){}
    }
  }catch(e){
    // TCP je padel med scanom (pričakovano ~6s) — počakamo in osvežimo
    await new Promise(r=>setTimeout(r,8000));
    try{await loadWifiStatus();}catch(_){}
  }
  if(btn){btn.textContent=orig;btn.disabled=false;}
}

async function wifiReconnectNow(){
  if(!confirm('Ročni WiFi reconnect? Kratka prekinitev (~10s).'))return;
  const r=await fetch('/api/wifi/reconnect',{method:'POST'});
  const m=document.getElementById('msgWifi');
  m.textContent=r.ok?'Reconnect izveden — posodabljam...':'Napaka!';
  m.style.color=r.ok?'#ff9500':'#ff3b30';
  setTimeout(async()=>{await loadWifi();m.textContent='';},3000);
}

async function wifiFactoryReset(){
  if(!confirm('Ponastavi WiFi omrežja na factory defaults? Obstoječi sloti se izbrišejo.'))return;
  const r=await fetch('/api/factory_reset',{method:'POST'});
  const m=document.getElementById('msgWifi');
  m.textContent=r.ok?'Ponastavljeno na privzeta omrežja!':'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(async()=>{await loadWifi();m.textContent='';},2000);
}

// ── Monitor nastavitve ─────────────────────────────────────────────
async function loadMon(){
  try{
    const s=await(await fetch('/api/monitorsettings')).json();
    document.getElementById('mIp').value=s.monitorIp;
    document.getElementById('mWth').value=s.wattThreshold;
    const t=document.getElementById('mPortTbl');
    t.innerHTML='<tr><th>Port</th><th>Ime</th><th>Enable</th></tr>';
    s.ports.forEach((p,i)=>{const tr=document.createElement('tr');tr.innerHTML=`<td><input type="number" class="mp" data-i="${i}" value="${p.port}" min="0" max="65535" style="width:65px"></td><td><input type="text" class="mn" data-i="${i}" value="${p.name}" maxlength="11" style="width:80px"></td><td><input type="checkbox" class="me" data-i="${i}"${p.enabled?' checked':''}></td>`;t.appendChild(tr);});
    _mL=true;
  }catch(e){}
}
async function saveMon(){
  const ports=[];
  for(let i=0;i<9;i++){const pp=document.querySelector(`.mp[data-i="${i}"]`),pn=document.querySelector(`.mn[data-i="${i}"]`),pe=document.querySelector(`.me[data-i="${i}"]`);if(pp)ports.push({port:parseInt(pp.value)||0,name:pn?pn.value:'',enabled:pe?pe.checked:false});}
  const b={monitorIp:document.getElementById('mIp').value,wattThreshold:parseFloat(document.getElementById('mWth').value)||3.0,ports};
  const r=await fetch('/save/monitor',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgM');m.textContent=r.ok?'Shranjeno!':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';setTimeout(()=>m.textContent='',3000);
}

// ── PWM kalibracija ────────────────────────────────────────────────
function updatePwmInvUI(inv){
  const lbl=document.getElementById('pwmInvLabel'),
        tog=document.getElementById('pwmInvToggle'),
        tr=document.getElementById('pwmInvTrack'),
        kn=document.getElementById('pwmInvKnob');
  tog.checked=inv;
  lbl.textContent=inv?'INVERT':'NORMAL';
  lbl.style.color=inv?'#ff9500':'#30d158';
  tr.style.background=inv?'#ff9500':'#2a2a2a';
  kn.style.left=inv?'23px':'3px';
}
function onPwmInvToggle(){
  updatePwmInvUI(document.getElementById('pwmInvToggle').checked);
}
async function savePwmCal(){
  const freq=parseInt(document.getElementById('pwmFreq').value)||25000;
  const inv=document.getElementById('pwmInvToggle').checked;
  if(freq<10||freq>50000){
    const m=document.getElementById('msgPwm');
    m.textContent='Frekvenca mora biti med 10 in 50000 Hz!';
    m.style.color='#ff3b30';
    setTimeout(()=>m.textContent='',4000);
    return;
  }
  const r=await fetch('/save/pwmcal',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({fanPwmFreq:freq,fanPwmInvert:inv})});
  const m=document.getElementById('msgPwm');
  m.textContent=r.ok?'Shranjeno in aktivirano!':'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',4000);
}

// ── LED toggle ─────────────────────────────────────────────────────
async function loadLed(){
  try{
    const d=await(await fetch('/api/led')).json();
    updateLedUI(d.enabled);
  }catch(e){}
}
function updateLedUI(en){
  const lbl=document.getElementById('ledLabel'),
        tog=document.getElementById('ledToggle'),
        tr=document.getElementById('ledTrack'),
        kn=document.getElementById('ledKnob');
  tog.checked=en;
  lbl.textContent=en?'ENABLED':'DISABLED';
  lbl.style.color=en?'#30d158':'#555';
  tr.style.background=en?'#30d158':'#2a2a2a';
  kn.style.left=en?'23px':'3px';
}
async function onLedToggle(){
  const en=document.getElementById('ledToggle').checked;
  updateLedUI(en);
  const r=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:en})});
  const m=document.getElementById('msgLed');
  m.textContent=r.ok?(en?'LED vklopljena':'LED izklopljena'):'Napaka!';
  m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
}

// ── Sistemske informacije ──────────────────────────────────────────
function buildSysInfo(d){
  const si=document.getElementById('sysinfo');
  if(!si)return;
  const hFree=d.heap_free||0,hTotal=d.heap_total||0,hMin=d.heap_min||0;
  const pFree=d.psram_free||0,pTotal=d.psram_total||0;
  const rows=[
    ['IP',d.ip||'--'],
    ['MAC',d.mac||'--'],
    ['RSSI',d.rssi?d.rssi+' dBm':'--'],
    ['Uptime',fUp(d.uptime||0)],
    ['Firmware',d.fw||'--'],
    ['Chip',d.chip_model?(d.chip_model+' rev'+d.chip_rev):'--'],
    ['CPU',d.cpu_freq?d.cpu_freq+' MHz':'--'],
    ['Flash',fBytes(d.flash_size||0)],
    ['Heap free',fBytes(hFree)+' ('+fPct(hFree,hTotal)+')'],
    ['Heap min ever',fBytes(hMin)],
    ['PSRAM free',fBytes(pFree)+' ('+fPct(pFree,pTotal)+')'],
    ['PSRAM total',fBytes(pTotal)],
    ['Napake',d.err===0?'<span class="eb eok">OK</span>':'<span class="eb efail">ERR 0x'+d.err.toString(16).toUpperCase()+'</span>'],
  ];
  si.innerHTML=rows.map(([k,v])=>`<div class="ir"><span class="ik">${k}</span><span class="iv">${v}</span></div>`).join('');
}
async function fetchSys(){
  try{const d=await(await fetch('/api/data')).json();buildSysInfo(d);}catch(e){}
}

// ── RAM Log s filtrom ──────────────────────────────────────────────
let _allLogs=[];
async function fetchLog(){
  try{
    _allLogs=await(await fetch('/api/log')).json();
    applyLogFilter();
  }catch(e){}
}
function applyLogFilter(){
  const showI=document.getElementById('fInfo').checked,
        showW=document.getElementById('fWarn').checked,
        showE=document.getElementById('fErr').checked;
  const t=document.getElementById('logtbl');
  t.innerHTML='<tr><th>Čas</th><th>Lvl</th><th>Tag</th><th>Sporočilo</th></tr>';
  _allLogs.forEach(e=>{
    if(e.level==='INFO'&&!showI)return;
    if(e.level==='WARN'&&!showW)return;
    if(e.level==='ERROR'&&!showE)return;
    const cls=e.level==='ERROR'?'le':e.level==='WARN'?'lw':'li';
    const tr=document.createElement('tr');tr.className=cls;
    tr.innerHTML=`<td>${e.time}</td><td>${e.level}</td><td>${e.tag}</td><td>${e.msg}</td>`;
    t.appendChild(tr);
  });
  const box=document.getElementById('logbox');
  if(box)box.scrollTop=box.scrollHeight;
}
async function clearLog(){
  try{await fetch('/api/log/clear',{method:'POST'});_allLogs=[];applyLogFilter();}catch(e){}
}
function _triggerDownload(blob,fname){
  const u=URL.createObjectURL(blob);
  const a=document.createElement('a');
  a.href=u;a.download=fname;a.click();
  URL.revokeObjectURL(u);
}
function _tsStr(){
  const d=new Date(),p=n=>String(n).padStart(2,'0');
  return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+'_'+p(d.getHours())+'-'+p(d.getMinutes());
}
function _fnameFromCD(cd,fallback){
  if(cd){const m=cd.match(/filename="?([^";]+)"?/);if(m)return m[1];}
  return fallback;
}
function downloadCSV(){
  const a=document.createElement('a');
  a.href='/api/history?format=csv';
  a.download='fancontrol_'+_tsStr()+'.csv';
  a.click();
}
function downloadLog(){
  const a=document.createElement('a');
  a.href='/api/log?format=txt';
  a.download='fancontrol_ramlog_'+_tsStr()+'.txt';
  a.click();
}

// ── Manual fan ─────────────────────────────────────────────────────
function updateManUI(isMan,pct){
  document.getElementById('manToggle').checked=isMan;
  const lbl=document.getElementById('modeLabel'),wrap=document.getElementById('manSliderWrap'),
        btn=document.getElementById('manApplyBtn'),knob=document.getElementById('manKnob'),
        track=document.getElementById('manSliderToggle');
  if(isMan){lbl.textContent='ROČNO';lbl.style.color='#ff6b00';wrap.style.display='';btn.style.display='';track.style.background='#ff6b00';knob.style.left='23px';}
  else{lbl.textContent='AVTOMATSKO';lbl.style.color='#aaa';wrap.style.display='none';btn.style.display='none';track.style.background='#2a2a2a';knob.style.left='3px';}
  if(pct!==undefined){document.getElementById('manPct').value=pct;document.getElementById('manPctVal').textContent=pct+'%';}
}
function onManToggle(){
  const tog=document.getElementById('manToggle');
  const isOn=tog.checked;
  updateManUI(isOn);
  if(!isOn){
    fetch('/api/fan/manual',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({manual:false,pct:0})})
      .then(r=>{
        const m=document.getElementById('msgMan');
        m.textContent=r.ok?'Avtomatsko':'Napaka!';
        m.style.color=r.ok?'#30d158':'#ff3b30';
        setTimeout(()=>m.textContent='',2000);
      });
  } else {
    const pct=parseInt(document.getElementById('manPct').value)||0;
    fetch('/api/fan/manual',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({manual:true,pct:pct})})
      .then(r=>{
        const m=document.getElementById('msgMan');
        m.textContent=r.ok?'Ročno — nastavljeno '+pct+'%':'Napaka!';
        m.style.color=r.ok?'#ff6b00':'#ff3b30';
        setTimeout(()=>m.textContent='',2000);
      });
  }
}
function onManSlider(){const v=document.getElementById('manPct').value;document.getElementById('manPctVal').textContent=v+'%';}
async function applyManual(){
  const pct=parseInt(document.getElementById('manPct').value)||0;
  const r=await fetch('/api/fan/manual',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({manual:true,pct:pct})});
  const m=document.getElementById('msgMan');m.textContent=r.ok?'Nastavljeno '+pct+'%':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';setTimeout(()=>m.textContent='',3000);
}

// ── OTA ────────────────────────────────────────────────────────────
document.getElementById('otaF').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('otaBin').files[0];if(!f)return;
  const btn=this.querySelector('button[type=submit]'),bar=document.getElementById('otaBar'),
        prg=document.getElementById('otaPrg'),st=document.getElementById('otaSt');
  btn.disabled=true;prg.style.display='block';st.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  let _otaDone=false;
  xhr.upload.onprogress=function(ev){if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);bar.style.width=p+'%';st.textContent='Nalaganje: '+p+'%';if(p>=100)_otaDone=true;}};
  xhr.onload=function(){if(xhr.status===200){bar.style.width='100%';bar.style.background='#30d158';st.textContent='Uspelo! Naprava se resetira v 5s...';setTimeout(()=>{location.href='/';},5500);}else{bar.style.background='#ff3b30';st.textContent='Napaka: '+xhr.responseText;btn.disabled=false;}};
  xhr.onerror=function(){if(_otaDone){bar.style.width='100%';bar.style.background='#30d158';st.textContent='Firmware naložen — naprava se je resetirala. Osvežujem...';setTimeout(()=>{location.href='/';},5000);}else{st.textContent='Napaka pri prenosu!';btn.disabled=false;}};
  const fd=new FormData();fd.append('update',f);xhr.open('POST','/update');xhr.send(fd);
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
        wifiScanHistoryAdd(WIFI_EVENT_BOOT,
                           WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                           -120, WiFi.RSSI());
        syncNTP();
        MDNS.begin(MDNS_HOSTNAME);
        LOG_INFO("MDNS", "http://%s.local", MDNS_HOSTNAME);
    }

    // --- GET / → glavna SPA stran (gzip kompresija, src/webserver_html_gz.h) ---
    // Če header manjka: python generate_html_gz.py (iz korenskega direktorija)
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *resp = request->beginResponse(
            200, "text/html; charset=UTF-8", MAIN_HTML_GZ, MAIN_HTML_GZ_LEN);
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Cache-Control", "no-cache");
        request->send(resp);
    });

    // --- GET /uplot.js → uPlot knjižnica iz PROGMEM ---
    server.on("/uplot.js", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *resp = request->beginResponse(
            200, "application/javascript", UPLOT_JS);
        resp->addHeader("Cache-Control", "public, max-age=86400");
        request->send(resp);
    });

    // --- GET /uplot.css → uPlot CSS iz PROGMEM ---
    server.on("/uplot.css", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *resp = request->beginResponse(
            200, "text/css", UPLOT_CSS);
        resp->addHeader("Cache-Control", "public, max-age=86400");
        request->send(resp);
    });

    // --- GET /api/data → JSON trenutnih vrednosti ---
    server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<1024> doc;
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

        // Sistemski podatki za Tab 3
        doc["heap_free"]     = ESP.getFreeHeap();
        doc["heap_total"]    = ESP.getHeapSize();
        doc["heap_min"]      = ESP.getMinFreeHeap();
        doc["psram_free"]    = ESP.getFreePsram();
        doc["psram_total"]   = ESP.getPsramSize();
        doc["cpu_freq"]      = ESP.getCpuFreqMHz();
        doc["flash_size"]    = ESP.getFlashChipSize();
        doc["chip_model"]    = ESP.getChipModel();
        doc["chip_rev"]      = ESP.getChipRevision();
        doc["mac"]           = WiFi.macAddress();
        doc["led_enabled"]   = settings.ledEnabled;
        doc["boost_active"]  = boostIsActive();
        doc["boost_pct"]     = boostGetPct();
        doc["boost_watt_thr"] = settings.boostWattThreshold;
        doc["boost_learn_ms"] = settings.boostLearnMs;

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/history?from=<ts>&to=<ts>[&maxPts=<n>][&format=csv] → JSON ali CSV ---
    // format=csv: streaming CSV download vseh točk brez decimacije
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request){
        // --- CSV streaming branch ---
        if (request->hasParam("format") &&
            request->getParam("format")->value() == "csv") {
            int cnt = graphGetCount();
            char fname[64];
            if (timeSynced) {
                snprintf(fname, sizeof(fname),
                         "fancontrol_%04d-%02d-%02d_%02d-%02d.csv",
                         myTZ.year(), myTZ.month(), myTZ.day(),
                         myTZ.hour(), myTZ.minute());
            } else {
                snprintf(fname, sizeof(fname), "fancontrol_uptime_%lus.csv", millis()/1000);
            }
            char disp[80];
            snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
            AsyncResponseStream *resp = request->beginResponseStream("text/csv; charset=UTF-8");
            resp->addHeader("Content-Disposition", disp);
            resp->print("timestamp,datetime,temp_c,hum_pct,watt,fan_pct\r\n");
            for (int i = 0; i < cnt; i++) {
                GraphPoint p = graphGetPoint(i);
                if (p.ts == 0) continue;
                char dtbuf[20];
                if (timeSynced) {
                    time_t t = (time_t)p.ts;
                    struct tm tm_info;
                    gmtime_r(&t, &tm_info);
                    snprintf(dtbuf, sizeof(dtbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                             tm_info.tm_year+1900, tm_info.tm_mon+1, tm_info.tm_mday,
                             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
                } else {
                    snprintf(dtbuf, sizeof(dtbuf), "no_time");
                }
                char line[96];
                snprintf(line, sizeof(line), "%lu,%s,%.1f,%.0f,%.1f,%u\r\n",
                         (unsigned long)p.ts, dtbuf,
                         p.temp, p.hum, p.watt, (unsigned)p.fanPct);
                resp->print(line);
            }
            request->send(resp);
            LOG_INFO("WEB", "/api/history?format=csv — %d tock", cnt);
            return;
        }

        // --- JSON branch (grafi) ---
        uint32_t fromTs = 0;
        uint32_t toTs   = 0xFFFFFFFF;
        int maxPts = 0;

        if (request->hasParam("from")) {
            fromTs = (uint32_t)atol(request->getParam("from")->value().c_str());
        }
        if (request->hasParam("to")) {
            toTs = (uint32_t)atol(request->getParam("to")->value().c_str());
        }
        if (request->hasParam("maxPts")) {
            maxPts = atoi(request->getParam("maxPts")->value().c_str());
        }

        int cnt = graphGetCount();
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("[");

        struct BucketAcc {
            double sumTs, sumTemp, sumHum, sumWatt, sumFan;
            int count;
        };

        bool decimated = false;
        if (maxPts > 0 && toTs > fromTs) {
            int winCount = 0;
            for (int i = 0; i < cnt; i++) {
                GraphPoint p = graphGetPoint(i);
                if (p.ts >= fromTs && p.ts <= toTs) winCount++;
            }
            if (winCount > maxPts) {
                int capped = (maxPts > 2000) ? 2000 : maxPts;
                BucketAcc* buckets = (BucketAcc*)ps_malloc(sizeof(BucketAcc) * capped);
                if (buckets) {
                    memset(buckets, 0, sizeof(BucketAcc) * capped);
                    int64_t range = (int64_t)toTs - (int64_t)fromTs + 1;
                    for (int i = 0; i < cnt; i++) {
                        GraphPoint p = graphGetPoint(i);
                        if (p.ts < fromTs || p.ts > toTs) continue;
                        int bi = (int)((int64_t)(p.ts - fromTs) * capped / range);
                        if (bi >= capped) bi = capped - 1;
                        buckets[bi].sumTs   += p.ts;
                        buckets[bi].sumTemp += p.temp;
                        buckets[bi].sumHum  += p.hum;
                        buckets[bi].sumWatt += p.watt;
                        buckets[bi].sumFan  += p.fanPct;
                        buckets[bi].count++;
                    }
                    bool first = true;
                    for (int i = 0; i < capped; i++) {
                        if (!buckets[i].count) continue;
                        int c = buckets[i].count;
                        if (!first) resp->print(",");
                        first = false;
                        char buf[96];
                        snprintf(buf, sizeof(buf),
                                 "{\"ts\":%lu,\"temp\":%.1f,\"hum\":%.0f,\"watt\":%.1f,\"fan\":%u}",
                                 (unsigned long)(uint32_t)(buckets[i].sumTs / c),
                                 buckets[i].sumTemp / c,
                                 buckets[i].sumHum / c,
                                 buckets[i].sumWatt / c,
                                 (unsigned)(int)(buckets[i].sumFan / c + 0.5));
                        resp->print(buf);
                    }
                    free(buckets);
                    decimated = true;
                }
            }
        }

        if (!decimated) {
            bool first = true;
            for (int i = 0; i < cnt; i++) {
                GraphPoint p = graphGetPoint(i);
                if (p.ts < fromTs || p.ts > toTs) continue;
                if (!first) resp->print(",");
                first = false;
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "{\"ts\":%lu,\"temp\":%.1f,\"hum\":%.0f,\"watt\":%.1f,\"fan\":%u}",
                         (unsigned long)p.ts, p.temp, p.hum, p.watt, (unsigned)p.fanPct);
                resp->print(buf);
            }
        }

        resp->print("]");
        request->send(resp);
    });

    // --- GET /api/history/csv → chunked CSV download celotnega bufferja ---
    // Streaming po točkah — ESP32 RAM ni obremenjen z celotnim odgovorom.
    // Format: timestamp,datetime,temp_c,hum_pct,watt,fan_pct
    server.on("/api/history/csv", HTTP_GET, [](AsyncWebServerRequest *request){
        int cnt = graphGetCount();

        // Ime datoteke z datumom če je čas sinhroniziran
        char fname[64];
        if (timeSynced) {
            snprintf(fname, sizeof(fname),
                     "fancontrol_%04d-%02d-%02d_%02d-%02d.csv",
                     myTZ.year(), myTZ.month(), myTZ.day(),
                     myTZ.hour(), myTZ.minute());
        } else {
            unsigned long s = millis() / 1000;
            snprintf(fname, sizeof(fname), "fancontrol_uptime_%lus.csv", s);
        }

        char disp[80];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);

        AsyncResponseStream *resp = request->beginResponseStream("text/csv; charset=UTF-8");
        resp->addHeader("Content-Disposition", disp);

        // Glava CSV
        resp->print("timestamp,datetime,temp_c,hum_pct,watt,fan_pct\r\n");

        for (int i = 0; i < cnt; i++) {
            GraphPoint p = graphGetPoint(i);
            if (p.ts == 0) continue; // preskoči točke z neveljavnim timestampom

            char dtbuf[20];
            if (timeSynced) {
                time_t t = (time_t)p.ts;
                struct tm tm_info;
                gmtime_r(&t, &tm_info);
                snprintf(dtbuf, sizeof(dtbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                         tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            } else {
                snprintf(dtbuf, sizeof(dtbuf), "no_time");
            }

            char line[96];
            snprintf(line, sizeof(line),
                     "%lu,%s,%.1f,%.0f,%.1f,%u\r\n",
                     (unsigned long)p.ts, dtbuf,
                     p.temp, p.hum, p.watt, (unsigned)p.fanPct);
            resp->print(line);
        }
        request->send(resp);
        LOG_INFO("WEB", "/api/history/csv — %d tock, ime: %s", cnt, fname);
    });

    // --- GET /api/log[?format=txt] → JSON (zadnjih 50) ali streaming TXT download (vsi) ---
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request){
        int count = logGetCount();

        // --- TXT streaming branch (download) ---
        if (request->hasParam("format") &&
            request->getParam("format")->value() == "txt") {
            char fname[48];
            if (timeSynced) {
                snprintf(fname, sizeof(fname),
                         "fancontrol_ramlog_%04d-%02d-%02d_%02d-%02d.txt",
                         myTZ.year(), myTZ.month(), myTZ.day(),
                         myTZ.hour(), myTZ.minute());
            } else {
                snprintf(fname, sizeof(fname), "fancontrol_ramlog_uptime_%lus.txt", millis()/1000);
            }
            char disp[80];
            snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
            AsyncResponseStream *resp = request->beginResponseStream("text/plain; charset=UTF-8");
            resp->addHeader("Content-Disposition", disp);
            for (int i = 0; i < count; i++) {
                LogEntry e = logGetEntry(i);
                const char* lvl = (e.level == LOG_LVL_ERROR) ? "ERR" :
                                  (e.level == LOG_LVL_WARN)  ? "WRN" : "INF";
                char line[180];
                snprintf(line, sizeof(line), "[%s][%s][%s] %s\n",
                         e.time, lvl, e.tag, e.msg);
                resp->print(line);
            }
            request->send(resp);
            LOG_INFO("WEB", "/api/log?format=txt — %d vnosov", count);
            return;
        }

        // --- JSON branch (prikaz v UI, zadnjih 50) ---
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

    // --- GET /api/log/all → streaming JSON array VSEH vnosov (za download) ---
    server.on("/api/log/all", HTTP_GET, [](AsyncWebServerRequest *request){
        int count = logGetCount();
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("[");
        for (int i = 0; i < count; i++) {
            LogEntry e = logGetEntry(i);
            if (i > 0) resp->print(",");
            StaticJsonDocument<300> doc;
            doc["time"]  = e.time;
            doc["level"] = (e.level == LOG_LVL_ERROR) ? "ERROR" :
                           (e.level == LOG_LVL_WARN)  ? "WARN"  : "INFO";
            doc["tag"]   = e.tag;
            doc["msg"]   = e.msg;
            String s; serializeJson(doc, s);
            resp->print(s);
        }
        resp->print("]");
        request->send(resp);
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
        StaticJsonDocument<768> doc;
        JsonArray ct     = doc.createNestedArray("curveTemp");
        JsonArray cp     = doc.createNestedArray("curvePct");
        JsonArray locked = doc.createNestedArray("curveLocked");
        JsonArray conf   = doc.createNestedArray("curveConfidence");
        for (int i = 0; i < FAN_CURVE_POINTS; i++) {
            ct.add(settings.curveTemp[i]);
            cp.add(settings.curvePct[i]);
            locked.add(settings.curveLocked[i]);
            conf.add(settings.curveConfidence[i]);
        }
        doc["fanStartPct"]  = settings.fanStartPct;
        doc["fanStopPct"]   = settings.fanStopPct;
        doc["fanMaxDayPct"] = settings.fanMaxDayPct;
        doc["dndMaxPct"]    = settings.dndMaxPct;
        doc["dndEnabled"]   = settings.dndEnabled;
        doc["dndFrom"]      = settings.dndFrom;
        doc["dndTo"]        = settings.dndTo;
        doc["adaptThresh"]        = ADAPT_CONFIDENCE_THRESH;
        doc["boostWattThreshold"] = settings.boostWattThreshold;
        doc["boostPct"]           = settings.boostPct;
        doc["boostLocked"]        = settings.boostLocked;
        doc["boostEvalSec"]       = settings.boostEvalMs / 1000UL;
        doc["boostLearnSec"]      = settings.boostLearnMs / 1000UL;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/calsettings → za pre-fill Tab 2 ---
    server.on("/api/calsettings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<384> doc;
        doc["tempOffset"]  = settings.tempOffset;
        doc["humOffset"]   = settings.humOffset;
        doc["shuntOhms"]   = settings.shuntOhms;
        doc["currentCorr"] = settings.currentCorr;
        doc["ssid"]        = settings.ssid;
        doc["fanPwmFreq"]   = settings.fanPwmFreq;
        doc["fanPwmInvert"] = settings.fanPwmInvert;
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

            StaticJsonDocument<768> doc;
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
            // Zaklep točk
            JsonArray lk = doc["curveLocked"];
            if (lk && lk.size() == FAN_CURVE_POINTS) {
                for (int i = 0; i < FAN_CURVE_POINTS; i++) {
                    settings.curveLocked[i] = lk[i];
                }
            }
            if (doc.containsKey("fanStartPct") || doc.containsKey("fanStopPct")) {
                uint8_t newStart = (uint8_t)constrain(
                    doc["fanStartPct"] | (int)settings.fanStartPct, 1, 100);
                uint8_t newStop = (uint8_t)constrain(
                    doc["fanStopPct"] | (int)settings.fanStopPct, 0, 99);
                if ((int)newStart - (int)newStop < 1) {
                    request->send(400, "application/json",
                        "{\"error\":\"fanStopPct mora biti vsaj 1% nizji od fanStartPct\"}");
                    return;
                }
                settings.fanStartPct = newStart;
                settings.fanStopPct  = newStop;
            }
            if (doc.containsKey("fanMaxDayPct")) {
                uint8_t v = doc["fanMaxDayPct"];
                if (v <= 100) settings.fanMaxDayPct = v;
            }
            if (doc.containsKey("dndMaxPct")) {
                uint8_t v = (uint8_t)constrain((int)(doc["dndMaxPct"] | 0), 0, 100);
                // fanStartPct je %PWM — pretvorba v %user za primerjavo z dndMaxPct (%user)
                // Formula: user = 1 + (pwm - 27) * 99 / 73  (identična _pwmToUser v fan.cpp)
                uint8_t fanStartUser = (settings.fanStartPct <= 27) ? 1 :
                    (uint8_t)(1.0f + (float)((int)settings.fanStartPct - 27) * 99.0f / 73.0f + 0.5f);
                // Veljavno: 0% (fan ugasnjen med DND) ALI >= fanStartUser%user
                if (v > 0 && v < fanStartUser) {
                    request->send(400, "application/json",
                        "{\"error\":\"DND max mora biti 0% ali vsaj toliko kot prag zagona ventilatorja\"}");
                    return;
                }
                settings.dndMaxPct = v;
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
            if (doc.containsKey("boostWattThreshold")) {
                float v = doc["boostWattThreshold"];
                if (v >= 1.0f && v <= 50.0f) settings.boostWattThreshold = v;
            }
            if (doc.containsKey("boostPct")) {
                uint8_t v = doc["boostPct"];
                if (v >= BOOST_PCT_MIN && v <= BOOST_PCT_MAX) settings.boostPct = v;
            }
            if (doc.containsKey("boostLocked")) {
                settings.boostLocked = doc["boostLocked"];
            }
            if (doc.containsKey("boostEvalSec")) {
                uint32_t v = (uint32_t)(int)doc["boostEvalSec"];
                if (v >= 5 && v <= 300) settings.boostEvalMs = v * 1000UL;
            }
            if (doc.containsKey("boostLearnSec")) {
                uint32_t v = (uint32_t)(int)doc["boostLearnSec"];
                if (v >= 30 && v <= 600) settings.boostLearnMs = v * 1000UL;
            }

            saveSettings();
            LOG_INFO("WEB", "/save/fan OK");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- POST /adapt/reset → reset učenja na default ---
    server.on("/adapt/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        adaptReset();
        LOG_INFO("WEB", "/adapt/reset — ucenje ponastavljeno");
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });

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
        float  liveWatt   = sensorData.watt;
        uint8_t liveErr   = sensorData.err;
        float  wattThr    = settings.monitorWattThreshold;
        portEXIT_CRITICAL(&dataMux);

        // powered izračunamo v real-time iz trenutnega watt (ne čakamo 5-min TCP scan)
        bool powered = !(liveErr & ERR_INA219) && (liveWatt >= wattThr);

        StaticJsonDocument<1024> doc;
        doc["powered"]    = powered;
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
            if (ok) { delay(1500); ESP.restart(); }
        },
        [](AsyncWebServerRequest *request, String filename,
           size_t index, uint8_t *data, size_t len, bool final){
            if (!index) {
                LOG_INFO("OTA", "Start: %s (%u B)",
                         filename.c_str(), (unsigned)request->contentLength());
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

    // --- POST /save/pwmcal → shrani PWM kalibracijo in takoj reinit ---
    server.on("/save/pwmcal", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char _pwmBuf[128];
            static size_t _pwmLen = 0;
            if (index == 0) { _pwmLen = 0; memset(_pwmBuf, 0, sizeof(_pwmBuf)); }
            size_t cp = (len < sizeof(_pwmBuf) - 1 - _pwmLen) ? len : sizeof(_pwmBuf) - 1 - _pwmLen;
            memcpy(_pwmBuf + _pwmLen, data, cp);
            _pwmLen += cp;
            if (index + len != total) return;

            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, _pwmBuf)) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            if (doc.containsKey("fanPwmFreq")) {
                uint32_t v = (uint32_t)(unsigned long)doc["fanPwmFreq"];
                if (v >= 10 && v <= 50000) settings.fanPwmFreq = v;
            }
            if (doc.containsKey("fanPwmInvert")) {
                settings.fanPwmInvert = doc["fanPwmInvert"] | false;
            }
            saveSettings();
            reinitFanPwm();
            LOG_INFO("WEB", "/save/pwmcal — freq=%lu invert=%s",
                     (unsigned long)settings.fanPwmFreq,
                     settings.fanPwmInvert ? "ON" : "off");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- GET /api/led → vrni stanje LED ---
    server.on("/api/led", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<32> doc;
        doc["enabled"] = settings.ledEnabled;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /api/led → nastavi stanje LED ---
    server.on("/api/led", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char*  _ledBuf = nullptr;
            static size_t _ledLen = 0;
            if (index == 0) {
                if (!_ledBuf) _ledBuf = (char*)ps_malloc(256);
                if (!_ledBuf) { request->send(500, "text/plain", "OOM"); return; }
                _ledLen = 0;
                memset(_ledBuf, 0, 256);
            }
            size_t cp = min(len, (size_t)(255 - _ledLen));
            memcpy(_ledBuf + _ledLen, data, cp);
            _ledLen += cp;
            if (index + len != total) return;

            StaticJsonDocument<32> doc;
            if (deserializeJson(doc, _ledBuf)) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            settings.ledEnabled = doc["enabled"] | true;
            saveSettings();
            if (!settings.ledEnabled) {
                ledOff();
            } else {
                if (sensorData.err == ERR_NONE)      ledGreen();
                else if (sensorData.err & ERR_WIFI)  ledRed();
                else                                  ledOrange();
            }
            LOG_INFO("WEB", "/api/led — enabled=%s", settings.ledEnabled ? "true" : "false");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- POST /api/restart → soft reset z 500ms zakasnitvijo ---
    server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", "{\"status\":\"restarting\"}");
        LOG_INFO("WEB", "/api/restart — zahtevam restart");
        delay(500);
        ESP.restart();
    });

    // --- POST /api/settings/reset → ponastavi nastavitve na privzete vrednosti in restart ---
    server.on("/api/settings/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("WEB", "/api/settings/reset — ponastavitev na privzete vrednosti");
        resetSettings();
        request->send(200, "application/json", "{\"status\":\"reset\"}");
        delay(500);
        ESP.restart();
    });

    // --- GET /api/wifi/status → trenutno stanje + scan rezultati ---
    server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request){
        bool connected = (WiFi.status() == WL_CONNECTED);
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("{");
        resp->printf("\"connected\":%s,", connected ? "true" : "false");
        if (connected) {
            resp->printf("\"ssid\":\"%s\",", WiFi.SSID().c_str());
            resp->printf("\"bssid\":\"%s\",", WiFi.BSSIDstr().c_str());
            resp->printf("\"channel\":%d,", WiFi.channel());
            resp->printf("\"rssi\":%d,", WiFi.RSSI());
        } else {
            resp->print("\"ssid\":\"\",\"bssid\":\"\",\"channel\":0,\"rssi\":0,");
        }
        uint32_t scanAge = wifiLastScanMs > 0 ? (millis() - wifiLastScanMs) / 1000 : 0;
        resp->printf("\"last_scan_s\":%lu,", (unsigned long)scanAge);
        resp->printf("\"scan_in_progress\":%s,", wifiScanInProgress ? "true" : "false");
        resp->print("\"aps\":[");
        for (uint8_t i = 0; i < wifiScanResultCount; i++) {
            if (i > 0) resp->print(",");
            resp->printf("{\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"isConnected\":%s}",
                         wifiScanResults[i].ssid, wifiScanResults[i].bssid,
                         wifiScanResults[i].rssi, wifiScanResults[i].channel,
                         wifiScanResults[i].isConnected ? "true" : "false");
        }
        resp->print("]}");
        request->send(resp);
    });

    // --- GET /api/wifi/history → zadnjih 10 scan/reconnect eventov ---
    server.on("/api/wifi/history", HTTP_GET, [](AsyncWebServerRequest *request){
        const char* typeNames[] = {"scan","reconnect","manual","boot"};
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("[");
        // Izpiši v kronološkem vrstnem redu (najstarejši → najnovejši)
        uint8_t cnt = wifiScanHistoryCount;
        uint8_t startIdx = (cnt < WIFI_SCAN_HISTORY_SIZE)
                           ? 0
                           : (wifiScanHistoryIdx); // začetek pri prvem (najstarejšem)
        for (uint8_t i = 0; i < cnt; i++) {
            uint8_t idx = (startIdx + i) % WIFI_SCAN_HISTORY_SIZE;
            WifiScanEntry& e = wifiScanHistory[idx];
            if (i > 0) resp->print(",");
            const char* tn = (e.type < 4) ? typeNames[e.type] : "?";
            resp->printf("{\"ts\":%lu,\"type\":\"%s\",\"ssid\":\"%s\",\"bssid\":\"%s\","
                         "\"rssi_pre\":%d,\"rssi_post\":%d}",
                         (unsigned long)e.ts, tn,
                         e.ssid, e.bssid, e.rssiPre, e.rssiPost);
        }
        resp->print("]");
        request->send(resp);
    });

    // --- GET /api/wifi/networks → seznam slotov (gesla se NE vrnejo) ---
    server.on("/api/wifi/networks", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print("{\"slots\":[");
        for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
            if (i > 0) resp->print(",");
            bool hasPass = strlen(settings.wifiSlots[i].pass) > 0;
            resp->printf("{\"ssid\":\"%s\",\"enabled\":%s,\"isPreset\":%s,\"hasPass\":%s}",
                         settings.wifiSlots[i].ssid,
                         settings.wifiSlots[i].enabled ? "true" : "false",
                         settings.wifiSlots[i].isPreset ? "true" : "false",
                         hasPass ? "true" : "false");
        }
        resp->print("]}");
        request->send(resp);
    });

    // --- POST /api/wifi/slots → shrani nov seznam slotov ---
    server.on("/api/wifi/slots", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static char*  _wsBuf = nullptr;
            static size_t _wsLen = 0;
            if (index == 0) {
                if (!_wsBuf) _wsBuf = (char*)ps_malloc(2048);
                if (!_wsBuf) { request->send(500, "text/plain", "OOM"); return; }
                _wsLen = 0;
                memset(_wsBuf, 0, 2048);
            }
            size_t copyLen = min(len, (size_t)(2047 - _wsLen));
            memcpy(_wsBuf + _wsLen, data, copyLen);
            _wsLen += copyLen;
            if (index + len != total) return;

            DynamicJsonDocument doc(2048);
            DeserializationError err = deserializeJson(doc, _wsBuf);
            if (err) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            JsonArray slots = doc["slots"];
            if (!slots) {
                request->send(400, "application/json", "{\"error\":\"Manjka 'slots'\"}");
                return;
            }

            // Validacija: vsaj 1 slot z enabled=true in nepraznim SSID
            bool valid = false;
            int idx = 0;
            for (JsonObject slot : slots) {
                if (idx >= WIFI_SLOT_COUNT) break;
                const char* ssid = slot["ssid"] | "";
                bool enabled = slot["enabled"] | false;
                if (enabled && strlen(ssid) > 0) valid = true;
                idx++;
            }
            if (!valid) {
                request->send(400, "application/json", "{\"error\":\"Vsaj en slot mora biti aktiven\"}");
                return;
            }

            // Shrani slote
            idx = 0;
            for (JsonObject slot : slots) {
                if (idx >= WIFI_SLOT_COUNT) break;
                const char* ssid = slot["ssid"] | "";
                const char* pass = slot["pass"] | "";
                bool enabled  = slot["enabled"]  | false;
                bool isPreset = slot["isPreset"] | false;

                strncpy(settings.wifiSlots[idx].ssid, ssid, WIFI_SSID_MAX_LEN - 1);
                settings.wifiSlots[idx].ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
                // Geslo: prazno = ohrani obstoječe
                if (strlen(pass) > 0) {
                    strncpy(settings.wifiSlots[idx].pass, pass, WIFI_PASS_MAX_LEN - 1);
                    settings.wifiSlots[idx].pass[WIFI_PASS_MAX_LEN - 1] = '\0';
                }
                settings.wifiSlots[idx].enabled  = enabled;
                settings.wifiSlots[idx].isPreset  = isPreset;
                idx++;
            }

            saveWifiSlots();
            LOG_INFO("WEB", "/api/wifi/slots OK — %d slotov shranjenih", idx);
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- POST /api/wifi/reconnect → ročni reconnect iz UI ---
    server.on("/api/wifi/reconnect", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("WEB", "/api/wifi/reconnect — ročni reconnect");
        String ssidPre  = WiFi.SSID();
        int    rssiPre  = WiFi.RSSI();
        WiFi.disconnect();
        delay(500);
        wifiDoScan();
        connectWiFi();
        if (WiFi.status() == WL_CONNECTED) {
            MDNS.end();
            MDNS.begin(MDNS_HOSTNAME);
            if (!timeSynced) syncNTP();
            wifiScanHistoryAdd(WIFI_EVENT_MANUAL,
                               WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                               rssiPre, WiFi.RSSI());
            LOG_INFO("WIFI", "Ročni reconnect OK — RSSI: %d dBm", WiFi.RSSI());
        }
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });

    // --- POST /api/wifi/scan → sproži async scan (ne blokira AsyncWebServer) ---
    server.on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("WEB", "/api/wifi/scan — zahtevam async scan...");
        wifiScanRequested = true;
        request->send(200, "application/json", "{\"status\":\"scanning\"}");
    });

    // --- POST /api/wifi/connect?ssid=X → poveži na specifičen znan SSID ---
    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("ssid")) {
            request->send(400, "application/json", "{\"error\":\"missing ssid\"}");
            return;
        }
        String ssid = request->getParam("ssid")->value();
        // Preveri da SSID obstaja v slotih
        bool found = false;
        for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
            if (settings.wifiSlots[i].enabled && ssid == settings.wifiSlots[i].ssid) {
                found = true; break;
            }
        }
        if (!found) {
            request->send(403, "application/json", "{\"error\":\"unknown ssid\"}");
            return;
        }
        LOG_INFO("WEB", "/api/wifi/connect — zahtevam: '%s'", ssid.c_str());
        strncpy(wifiConnectTarget, ssid.c_str(), 31); wifiConnectTarget[31] = '\0';
        wifiConnectRequested = true;
        request->send(200, "application/json", "{\"status\":\"connecting\"}");
    });

    // --- POST /api/factory_reset → ponastavi WiFi slote na factory defaults ---
    server.on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("WEB", "/api/factory_reset — ponastavitev WiFi slotov");
        // Pobriši NVS WiFi slote
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
            char key[12];
            snprintf(key, sizeof(key), "ws%d_ssid", i); prefs.remove(key);
            snprintf(key, sizeof(key), "ws%d_pass", i); prefs.remove(key);
            snprintf(key, sizeof(key), "ws%d_en",   i); prefs.remove(key);
            snprintf(key, sizeof(key), "ws%d_preset",i); prefs.remove(key);
        }
        prefs.end();
        // Reinicializiraj iz factory defaults
        for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
            if (i < WIFI_NETWORK_COUNT) {
                strncpy(settings.wifiSlots[i].ssid, WIFI_SSID_LIST[i], WIFI_SSID_MAX_LEN - 1);
                strncpy(settings.wifiSlots[i].pass, WIFI_PASS_LIST[i], WIFI_PASS_MAX_LEN - 1);
                settings.wifiSlots[i].enabled  = true;
                settings.wifiSlots[i].isPreset = true;
            } else {
                memset(&settings.wifiSlots[i], 0, sizeof(WifiSlot));
            }
        }
        saveWifiSlots();
        LOG_INFO("WEB", "/api/factory_reset OK — %d slotov ponastavljenih", WIFI_NETWORK_COUNT);
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });

    server.begin();
    LOG_INFO("WEB", "Server started on %s", WIFI_STATIC_IP);
}

// =====================================================================
// handleWebserver — mDNS + NTP re-sync + WiFi watchdog (klic v loop)
// =====================================================================
void handleWebserver() {
    // ESP32 ESPmDNS teče v ozadju — update() ni potreben

    // UI scan: disconnect→scan→reconnect
    if (wifiScanRequested && !wifiScanInProgress) {
        wifiScanRequested = false;
        wifiScanInProgress = true;
        wifiUiScanAndReconnect();
        wifiScanInProgress = false;
    }
    // UI connect na specifičen SSID
    if (wifiConnectRequested && !wifiScanInProgress) {
        wifiConnectRequested = false;
        wifiScanInProgress = true;
        wifiConnectToSSID(wifiConnectTarget);
        wifiScanInProgress = false;
    }

    // NTP re-sync vsakih 30 min (normalno delovanje)
    if (timeSynced && millis() - lastNtpSyncMs >= NTP_UPDATE_INTERVAL) {
        syncNTP();
    }
    // NTP retry vsakih 5 min če ob zagonu ni uspelo
    if (!timeSynced && WiFi.status() == WL_CONNECTED &&
        millis() - lastNtpSyncMs >= NTP_RETRY_INTERVAL) {
        LOG_INFO("NTP", "Retry — ob zagonu ni uspelo");
        syncNTP();
    }

    // WiFi Health: samodejni reconnect ob kopičenju NTP napak
    if (wifiNtpFailCount >= WIFI_NTP_FAIL_THRESHOLD &&
        WiFi.status() == WL_CONNECTED) {
        LOG_WARN("WIFI", "Health: %d consecutive NTP napake — sprožam WiFi reconnect",
                 wifiNtpFailCount);
        wifiNtpFailCount = 0;
        wifiLastReconnectMs = millis();
        WiFi.disconnect();
        delay(500);
        connectWiFi();
        if (WiFi.status() == WL_CONNECTED) {
            MDNS.end();
            MDNS.begin(MDNS_HOSTNAME);
            if (!timeSynced) syncNTP();
            wifiScanHistoryAdd(WIFI_EVENT_RECONNECT,
                               WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                               -120, WiFi.RSSI());
            LOG_INFO("WIFI", "Health reconnect OK — reinit NTP + mDNS");
        }
    }

    // WiFi Roaming: periodični tihi reconnect vsakih 90 minut za boljšo mesh točko
    if (millis() - wifiLastRoamMs >= WIFI_ROAM_INTERVAL_MS &&
        WiFi.status() == WL_CONNECTED) {
        wifiLastRoamMs = millis();
        int    rssiPre   = WiFi.RSSI();
        String bssidPre  = WiFi.BSSIDstr();
        String ssidPre   = WiFi.SSID();

        LOG_INFO("WIFI", "Roaming reconnect — RSSI pred: %d dBm  BSSID: %s",
                 rssiPre, bssidPre.c_str());

        wifiDoScan();  // scan med reconnectom — ni blokirno za normalno delovanje
        WiFi.disconnect();
        delay(500);
        connectWiFi();

        if (WiFi.status() == WL_CONNECTED) {
            int    rssiPost  = WiFi.RSSI();
            String bssidPost = WiFi.BSSIDstr();

            wifiScanHistoryAdd(WIFI_EVENT_SCAN,
                               ssidPre.c_str(), bssidPost.c_str(),
                               rssiPre, rssiPost);

            LOG_INFO("WIFI", "Roaming OK — RSSI: %d→%d dBm  BSSID: %s→%s",
                     rssiPre, rssiPost, bssidPre.c_str(), bssidPost.c_str());

            MDNS.end();
            MDNS.begin(MDNS_HOSTNAME);
        }
    }

    // WiFi watchdog vsakih 10 min — reconnect če sploh ni connected
    if (millis() - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = millis();
        if (WiFi.status() != WL_CONNECTED) {
            LOG_WARN("WIFI", "Watchdog: reconnecting...");
            connectWiFi();
            if (WiFi.status() == WL_CONNECTED) {
                MDNS.end();
                if (MDNS.begin(MDNS_HOSTNAME)) {
                    LOG_INFO("MDNS", "mDNS restartan po reconnectu: http://%s.local", MDNS_HOSTNAME);
                } else {
                    LOG_WARN("MDNS", "mDNS restart neuspesen");
                }
                if (!timeSynced) syncNTP();
                wifiScanHistoryAdd(WIFI_EVENT_RECONNECT,
                                   WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                                   -120, WiFi.RSSI());
            }
        }
    }
}
