// globals.h — Globalne spremenljivke za fancontrol
#pragma once
#include "config.h"
#include <Preferences.h>
#include <ezTime.h>

// --- Vremenski podatki (Open-Meteo) ---
struct WeatherData {
    float    outTemp;       // Zunanja temperatura [°C]
    uint8_t  outHum;        // Zunanja relativna vlažnost [%]
    uint8_t  wxCode;        // WMO weather_code (0–99)
    bool     isNight;       // izračunano iz sunrise/sunset ob vsakem fetchWeather()
    bool     valid;         // false dokler ni prvi uspešen fetch
    uint8_t  err;           // 0=OK, 1=HTTP napaka, 2=parse napaka
    char     sunrise[6];    // "HH:MM" — čas sončnega vzhoda
    char     sunset[6];     // "HH:MM" — čas sončnega zahoda
};

// --- Senzorski podatki ---
struct SensorData {
    float    temp;      // Temperatura [°C] — SHT30
    float    hum;       // Relativna vlažnost [%] — SHT30
    float    volt;      // Napetost Mini PC [V] — INA219
    float    amp;       // Tok Mini PC [A] — INA219
    float    watt;      // Moč Mini PC [W] — INA219
    float    peakWatt;  // Maksimalna izmerjena moč od zagona [W] — za bar skalo
    uint8_t  fanPct;    // Hitrost ventilatorja [%]
    bool     dndActive;  // DND način aktiven
    bool     manualMode; // Ročni način ventilatorja aktiven
    uint8_t  manualPct;  // Ročna vrednost [%]
    uint8_t  err;        // Bitmask napak (ErrorFlag)
};

// --- WiFi slot (runtime lista omrežij, 5 slotov) ---
#define WIFI_SSID_MAX_LEN   32
#define WIFI_PASS_MAX_LEN   64

struct WifiSlot {
    char ssid[WIFI_SSID_MAX_LEN];
    char pass[WIFI_PASS_MAX_LEN];
    bool enabled;
    bool isPreset;  // true = factory default (iz wifi_config.h), false = ročni vnos
};

// --- Nastavitve (NVS) ---
struct Settings {
    // WiFi — legacy polje (za kompatibilnost, novo: wifiSlots[])
    char     ssid[32];
    char     password[64];
    // Temperaturna krivulja (6 točk)
    float    curveTemp[FAN_CURVE_POINTS];
    uint8_t  curvePct[FAN_CURVE_POINTS];
    // Adaptivna krivulja — zaklep posameznih točk
    bool     curveLocked[FAN_CURVE_POINTS];
    // Adaptivni confidence — koliko ravnovesnih opazovanj podpira vsako točko
    uint8_t  curveConfidence[FAN_CURVE_POINTS];
    // DND
    bool     dndEnabled;
    uint8_t  dndFrom;   // ura 0–23
    uint8_t  dndTo;     // ura 0–23
    uint8_t  dndMaxPct; // max % med DND
    // Hysteresis pragovi
    uint8_t  fanStartPct;   // %pwm — kick hitrost za fizični zagon motorja (default FAN_START_PCT)
    uint8_t  fanStopPct;    // %pwm — minimalna hitrost med tekom (default FAN_STOP_PCT)

    // Kalibracija SHT30
    float    tempOffset;      // Temperaturni offset [°C], privzeto 0.0
    float    humOffset;       // Vlažnostni offset [%], privzeto 0.0

    // Kalibracija INA219
    float    shuntOhms;       // Vrednost shunt upora [Ω], privzeto 0.1
    float    currentCorr;     // Korekcijski faktor toka, privzeto 1.0

    // Ventilator — razširjeno
    uint8_t  fanMaxDayPct;    // Max hitrost podnevi [%], privzeto 100

    // Mini PC Monitor
    char  monitorIp[16];                // default MONITOR_DEFAULT_IP
    float monitorWattThreshold;         // default MONITOR_DEFAULT_WATT_THR
    struct {
        uint16_t port;
        char     name[12];
        bool     enabled;
    } monitorPorts[MONITOR_MAX_PORTS];

    bool    ledEnabled;         // RGB LED omogočena (false = LED nikoli ne sveti)

    // Watt Boost
    float    boostWattThreshold;
    uint8_t  boostPct;
    bool     boostLocked;
    uint32_t boostEvalMs;
    uint32_t boostLearnMs;   // Eval okno za boost samoučenje [ms]

    // PWM kalibracija
    uint32_t fanPwmFreq;    // PWM frekvenca ventilatorja [Hz], privzeto 25000
    bool     fanPwmInvert;  // Invertiran delovni cikel (HIGH=stop, LOW=tece), privzeto false

    // WiFi omrežja — runtime lista (5 slotov, NVS)
    WifiSlot wifiSlots[WIFI_SLOT_COUNT];
};

// --- Watt Boost stanje (runtime, ne v NVS) ---
struct WattBoost {
    float    prevWatt;
    uint32_t aboveThreshSinceMs;
    bool     boostActive;
    uint32_t boostStartMs;
    float    tempAtBoostStart;
    bool     evalDone;
    uint32_t fadeStartMs;
    uint8_t  boostPctAtFadeStart;
};

// --- Adaptivni opazovalec (runtime, ne shranjevati v NVS) ---
struct AdaptObserver {
    float    tempHistory[90];
    uint8_t  fanHistory[90];
    uint8_t  histIdx;
    uint8_t  histCount;
    bool     equilibriumActive;
    uint32_t equilibriumStartMs;
    uint32_t lastUpdateMs;
};

// --- Extern deklaracije ---
extern SensorData    sensorData;
extern Settings      settings;
extern AdaptObserver adaptObserver;
extern WattBoost     wattBoost;
extern Timezone   myTZ;
extern bool       timeSynced;
extern bool       newSensorData;

// Timing
extern unsigned long lastSensorReadMs;
extern unsigned long lastGraphStoreMs;
extern unsigned long lastDisplayRefreshMs;
extern unsigned long lastWifiCheckMs;
extern unsigned long lastNtpSyncMs;
extern unsigned long lastMonitorMs;

// Mutex za thread-safety (Core 0 = web, Core 1 = senzorji/fan)
extern portMUX_TYPE dataMux;

// Peak tracker (samo RAM, brez NVS)
extern float peakTemp;    // Max temperatura od reseta

// WiFi health tracking (RAM, ne NVS)
extern uint8_t  wifiNtpFailCount;      // Zaporedne NTP sync napake
extern uint32_t wifiLastRoamMs;        // millis() zadnjega roaming reconnecta
extern uint32_t wifiLastReconnectMs;   // millis() zadnjega kateregakoli reconnecta

// Vremenski podatki in timing
extern WeatherData  weatherData;
extern unsigned long lastWeatherFetchMs;

// --- Funkcije ---
void initGlobals();
void loadSettings();
void saveSettings();
void resetSettings();
void saveWifiSlots();

