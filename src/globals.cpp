// globals.cpp — Definicije globalnih spremenljivk in NVS upravljanje za fancontrol

#include "globals.h"
#include "wifi_config.h"

// --- Senzorski podatki in nastavitve ---
SensorData   sensorData;
Settings     settings;
AdaptObserver adaptObserver = {};
WattBoost     wattBoost     = {};

// --- ezTime: timezone objekt ---
Timezone myTZ;

// --- Cas ---
bool timeSynced    = false;
bool newSensorData = false;

// --- Timing ---
unsigned long lastSensorReadMs     = 0;
unsigned long lastGraphStoreMs     = 0;
unsigned long lastMonitorMs        = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastWifiCheckMs      = 0;
unsigned long lastNtpSyncMs        = 0;

// --- Mutex za thread-safety ---
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// --- Peak trackerji (NVS persistentni) ---
float   peakTemp = -999.0f;
uint8_t peakFan  = 0;

// --- WiFi health tracking (RAM, ne NVS) ---
uint8_t  wifiNtpFailCount    = 0;
uint32_t wifiLastRoamMs      = 0;
uint32_t wifiLastReconnectMs = 0;

// --- Vremenski podatki ---
WeatherData  weatherData   = { 0.0f, 0, 0, false, false, 0, "--:--", "--:--" };
unsigned long lastWeatherFetchMs = 0;

// =============================================================================
// CRC16 za NVS zaščito
// =============================================================================
static uint16_t calculateCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// =============================================================================
// PRIVZETE VREDNOSTI
// =============================================================================
static void initDefaults() {
    memset(settings.ssid,     0, sizeof(settings.ssid));
    memset(settings.password, 0, sizeof(settings.password));

    settings.curveTemp[0] = FAN_CURVE_TEMP_0;  settings.curvePct[0] = FAN_CURVE_PCT_0;
    settings.curveTemp[1] = FAN_CURVE_TEMP_1;  settings.curvePct[1] = FAN_CURVE_PCT_1;
    settings.curveTemp[2] = FAN_CURVE_TEMP_2;  settings.curvePct[2] = FAN_CURVE_PCT_2;
    settings.curveTemp[3] = FAN_CURVE_TEMP_3;  settings.curvePct[3] = FAN_CURVE_PCT_3;
    settings.curveTemp[4] = FAN_CURVE_TEMP_4;  settings.curvePct[4] = FAN_CURVE_PCT_4;
    settings.curveTemp[5] = FAN_CURVE_TEMP_5;  settings.curvePct[5] = FAN_CURVE_PCT_5;
    for (int i = 0; i < FAN_CURVE_POINTS; i++) {
        settings.curveLocked[i]     = false;
        settings.curveConfidence[i] = 0;
    }

    settings.tempOffset   = 0.0f;
    settings.humOffset    = 0.0f;
    settings.shuntOhms    = 0.1f;
    settings.currentCorr  = 1.0f;
    settings.fanMaxDayPct = 100;

    settings.dndEnabled  = false;
    settings.dndFrom     = FAN_DND_HOUR_FROM;
    settings.dndTo       = FAN_DND_HOUR_TO;
    settings.dndMaxPct   = FAN_DND_MAX_PCT;
    settings.fanStartPct = FAN_START_PCT;
    settings.fanStopPct  = FAN_STOP_PCT;

    // Mini PC Monitor
    strncpy(settings.monitorIp, MONITOR_DEFAULT_IP, sizeof(settings.monitorIp) - 1);
    settings.monitorIp[sizeof(settings.monitorIp) - 1] = '\0';
    settings.monitorWattThreshold = MONITOR_DEFAULT_WATT_THR;

    static const struct { uint16_t port; const char name[12]; bool enabled; }
    kPorts[] = {
        {MONITOR_PORT_0},
        {MONITOR_PORT_1},
        {MONITOR_PORT_2},
        {MONITOR_PORT_3},
        {MONITOR_PORT_4},
        {MONITOR_PORT_5},
        {MONITOR_PORT_6},
        {MONITOR_PORT_7},
        {MONITOR_PORT_8},
    };
    constexpr int kPortCount = (int)(sizeof(kPorts) / sizeof(kPorts[0]));
    for (int i = 0; i < kPortCount && i < MONITOR_MAX_PORTS; i++) {
        settings.monitorPorts[i].port    = kPorts[i].port;
        strncpy(settings.monitorPorts[i].name, kPorts[i].name, sizeof(settings.monitorPorts[i].name) - 1);
        settings.monitorPorts[i].name[sizeof(settings.monitorPorts[i].name) - 1] = '\0';
        settings.monitorPorts[i].enabled = kPorts[i].enabled;
    }
    for (int i = kPortCount; i < MONITOR_MAX_PORTS; i++) {
        settings.monitorPorts[i].port    = 0;
        memset(settings.monitorPorts[i].name, 0, sizeof(settings.monitorPorts[i].name));
        settings.monitorPorts[i].enabled = false;
    }

    settings.ledEnabled = true;

    settings.boostWattThreshold = BOOST_WATT_THRESHOLD_DEFAULT;
    settings.boostPct           = BOOST_PCT_DEFAULT;
    settings.boostLocked        = false;
    settings.boostEvalMs        = BOOST_EVAL_MS_DEFAULT;
    settings.boostLearnMs       = BOOST_LEARN_MS_DEFAULT;
    settings.fanPwmFreq   = FAN_PWM_FREQ;
    settings.fanPwmInvert = false;

    settings.nanopiIntervalMs = NANOPI_FETCH_INTERVAL_DEFAULT;
    strncpy(settings.nanopiIp, NANOPI_DEFAULT_IP, sizeof(settings.nanopiIp) - 1);
    settings.nanopiIp[sizeof(settings.nanopiIp) - 1] = '\0';

    Serial.println("[Settings] Privzete vrednosti nastavljene");
}

// =============================================================================
// LOAD / SAVE / RESET SETTINGS
// =============================================================================
void loadSettings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t marker = prefs.getUChar("marker", 0);
    if (marker != 0xAB) {
        Serial.println("[Settings] NVS marker neveljaven — privzete vrednosti");
        prefs.end(); initDefaults(); saveSettings(); return;
    }
    size_t bytesRead = prefs.getBytes("cfg", &settings, sizeof(Settings));
    uint16_t storedCRC = prefs.getUShort("crc", 0);
    prefs.end();

    if (bytesRead != sizeof(Settings)) {
        Serial.printf("[Settings] NVS size mismatch (%d != %d) — privzete vrednosti\n",
                      bytesRead, sizeof(Settings));
        initDefaults(); saveSettings(); return;
    }
    uint16_t calcCRC = calculateCRC((uint8_t*)&settings, sizeof(Settings));
    if (calcCRC != storedCRC) {
        Serial.println("[Settings] NVS CRC napaka — privzete vrednosti");
        initDefaults(); saveSettings(); return;
    }
    Serial.printf("[Settings] Naložene iz NVS (CRC=0x%04X)\n", calcCRC);
}

void saveSettings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("marker", 0xAB);
    prefs.putBytes("cfg", &settings, sizeof(Settings));
    uint16_t crc = calculateCRC((uint8_t*)&settings, sizeof(Settings));
    prefs.putUShort("crc", crc);
    prefs.end();
    Serial.printf("[Settings] Shranjene v NVS (CRC=0x%04X)\n", crc);
}

void resetSettings() {
    Serial.println("[Settings] Reset na privzete vrednosti!");
    initDefaults(); saveSettings();
}

// =============================================================================
// SAVE WIFI SLOTS — shrani samo WiFi slote v NVS (ločeno od saveSettings)
// =============================================================================
void saveWifiSlots() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        char keySSID[12], keyPass[12], keyEn[10], keyPreset[12];
        snprintf(keySSID,   sizeof(keySSID),   "ws%d_ssid",   i);
        snprintf(keyPass,   sizeof(keyPass),   "ws%d_pass",   i);
        snprintf(keyEn,     sizeof(keyEn),     "ws%d_en",     i);
        snprintf(keyPreset, sizeof(keyPreset), "ws%d_preset", i);

        prefs.putString(keySSID,   settings.wifiSlots[i].ssid);
        prefs.putString(keyPass,   settings.wifiSlots[i].pass);
        prefs.putBool(keyEn,       settings.wifiSlots[i].enabled);
        prefs.putBool(keyPreset,   settings.wifiSlots[i].isPreset);
    }
    prefs.end();
    Serial.println("[WiFi] Sloti shranjeni v NVS");
}

// =============================================================================
// INIT GLOBALS
// =============================================================================
void initGlobals() {
    loadSettings();

    memset(&sensorData, 0, sizeof(SensorData));
    sensorData.temp     = ERR_FLOAT;
    sensorData.hum      = ERR_FLOAT;
    sensorData.volt     = ERR_FLOAT;
    sensorData.amp      = ERR_FLOAT;
    sensorData.watt     = ERR_FLOAT;
    sensorData.fanPct   = 0;
    sensorData.dndActive = false;
    sensorData.err      = ERR_NONE;

    newSensorData          = false;
    lastSensorReadMs       = 0;
    lastGraphStoreMs       = 0;
    lastDisplayRefreshMs   = 0;
    lastWifiCheckMs        = 0;
    lastNtpSyncMs          = 0;

    // Peak trackerji — naloži iz NVS (peakWatt bo naložen z initPeakWatt() v sensors.cpp)
    sensorData.peakWatt = PEAK_WATT_DEFAULT;
    {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, true);
        peakTemp = prefs.getFloat(PEAK_TEMP_NVS_KEY, -999.0f);
        peakFan  = prefs.getUChar(PEAK_FAN_NVS_KEY, 0);
        prefs.end();
    }

    // WiFi sloti — inicializiraj iz NVS ali factory defaults
    {
        Preferences wPrefs;
        wPrefs.begin(NVS_NAMESPACE, true);
        for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
            char keySSID[12], keyPass[12], keyEn[10], keyPreset[12];
            snprintf(keySSID,   sizeof(keySSID),   "ws%d_ssid",   i);
            snprintf(keyPass,   sizeof(keyPass),   "ws%d_pass",   i);
            snprintf(keyEn,     sizeof(keyEn),     "ws%d_en",     i);
            snprintf(keyPreset, sizeof(keyPreset), "ws%d_preset", i);

            String ssid = wPrefs.getString(keySSID, "");
            if (ssid.length() > 0) {
                // NVS vrednost obstaja — naloži
                strncpy(settings.wifiSlots[i].ssid, ssid.c_str(), WIFI_SSID_MAX_LEN - 1);
                settings.wifiSlots[i].ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
                String pass = wPrefs.getString(keyPass, "");
                strncpy(settings.wifiSlots[i].pass, pass.c_str(), WIFI_PASS_MAX_LEN - 1);
                settings.wifiSlots[i].pass[WIFI_PASS_MAX_LEN - 1] = '\0';
                settings.wifiSlots[i].enabled  = wPrefs.getBool(keyEn, true);
                settings.wifiSlots[i].isPreset = wPrefs.getBool(keyPreset, false);
            } else if (i < WIFI_NETWORK_COUNT) {
                // NVS prazen — naloži factory default iz wifi_config.h
                strncpy(settings.wifiSlots[i].ssid, WIFI_SSID_LIST[i], WIFI_SSID_MAX_LEN - 1);
                settings.wifiSlots[i].ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
                strncpy(settings.wifiSlots[i].pass, WIFI_PASS_LIST[i], WIFI_PASS_MAX_LEN - 1);
                settings.wifiSlots[i].pass[WIFI_PASS_MAX_LEN - 1] = '\0';
                settings.wifiSlots[i].enabled  = true;
                settings.wifiSlots[i].isPreset = true;
            } else {
                // Prazen slot
                memset(&settings.wifiSlots[i], 0, sizeof(WifiSlot));
                settings.wifiSlots[i].enabled  = false;
                settings.wifiSlots[i].isPreset = false;
            }
        }
        wPrefs.end();
    }

    Serial.println("[Globals] Init OK");
}
