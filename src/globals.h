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
    bool     isNight;       // true če je ura med 22:00–06:00 (luna ikona)
    bool     valid;         // false dokler ni prvi uspešen fetch
    uint8_t  err;           // 0=OK, 1=HTTP napaka, 2=parse napaka
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
    bool     dndActive; // DND način aktiven
    uint8_t  err;       // Bitmask napak (ErrorFlag)
};

// --- Nastavitve (NVS) ---
struct Settings {
    // WiFi
    char     ssid[32];
    char     password[64];
    // Temperaturna krivulja (4 točke)
    float    curveTemp[FAN_CURVE_POINTS];
    uint8_t  curvePct[FAN_CURVE_POINTS];
    // DND
    bool     dndEnabled;
    uint8_t  dndFrom;   // ura 0–23
    uint8_t  dndTo;     // ura 0–23
    uint8_t  dndMaxPct; // max % med DND
    // Minimalna hitrost
    uint8_t  fanMinPct;

    // Kalibracija SHT30
    float    tempOffset;      // Temperaturni offset [°C], privzeto 0.0
    float    humOffset;       // Vlažnostni offset [%], privzeto 0.0

    // Kalibracija INA219
    float    shuntOhms;       // Vrednost shunt upora [Ω], privzeto 0.1
    float    currentCorr;     // Korekcijski faktor toka, privzeto 1.0

    // Ventilator — razširjeno
    uint8_t  fanMaxDayPct;    // Max hitrost podnevi [%], privzeto 100
};

// --- Extern deklaracije ---
extern SensorData sensorData;
extern Settings   settings;
extern Timezone   myTZ;
extern bool       timeSynced;
extern bool       newSensorData;

// Timing
extern unsigned long lastSensorReadMs;
extern unsigned long lastGraphStoreMs;
extern unsigned long lastDisplayRefreshMs;
extern unsigned long lastWifiCheckMs;
extern unsigned long lastNtpSyncMs;

// Mutex za thread-safety (Core 0 = web, Core 1 = senzorji/fan)
extern portMUX_TYPE dataMux;

// Peak tracker (samo RAM, brez NVS)
extern float peakTemp;    // Max temperatura od reseta

// Vremenski podatki in timing
extern WeatherData  weatherData;
extern unsigned long lastWeatherFetchMs;

// --- Funkcije ---
void initGlobals();
void loadSettings();
void saveSettings();
void resetSettings();
