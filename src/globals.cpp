// globals.cpp — Definicije globalnih spremenljivk in NVS upravljanje za fancontrol

#include "globals.h"

// --- Senzorski podatki in nastavitve ---
SensorData sensorData;
Settings   settings;

// --- ezTime: timezone objekt ---
Timezone myTZ;

// --- Cas ---
bool timeSynced    = false;
bool newSensorData = false;

// --- Timing ---
unsigned long lastSensorReadMs     = 0;
unsigned long lastGraphStoreMs     = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastWifiCheckMs      = 0;
unsigned long lastNtpSyncMs        = 0;

// --- Mutex za thread-safety ---
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// --- Peak tracker (samo RAM, brez NVS) ---
float peakTemp = -999.0f;

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

    settings.curveTemp[0] = FAN_CURVE_TEMP_0;
    settings.curveTemp[1] = FAN_CURVE_TEMP_1;
    settings.curveTemp[2] = FAN_CURVE_TEMP_2;
    settings.curveTemp[3] = FAN_CURVE_TEMP_3;

    settings.curvePct[0]  = FAN_CURVE_PCT_0;
    settings.curvePct[1]  = FAN_CURVE_PCT_1;
    settings.curvePct[2]  = FAN_CURVE_PCT_2;
    settings.curvePct[3]  = FAN_CURVE_PCT_3;

    settings.tempOffset   = 0.0f;
    settings.humOffset    = 0.0f;
    settings.shuntOhms    = 0.1f;
    settings.currentCorr  = 1.0f;
    settings.fanMaxDayPct = 100;

    settings.dndEnabled = false;
    settings.dndFrom    = FAN_DND_HOUR_FROM;
    settings.dndTo      = FAN_DND_HOUR_TO;
    settings.dndMaxPct  = FAN_DND_MAX_PCT;
    settings.fanMinPct  = FAN_MIN_PCT;

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

    // Peak watt — naloži iz NVS (persistentna vrednost)
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    sensorData.peakWatt = prefs.getFloat(PEAK_WATT_NVS_KEY, PEAK_WATT_DEFAULT);
    if (sensorData.peakWatt < PEAK_WATT_MIN_FLOOR) sensorData.peakWatt = PEAK_WATT_DEFAULT;
    prefs.end();

    Serial.println("[Globals] Init OK");
}
