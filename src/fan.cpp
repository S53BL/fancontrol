// fan.cpp — Implementacija PWM krmiljenja ventilatorja
#include "fan.h"
#include "globals.h"
#include "logging.h"
#include <Arduino.h>

static uint8_t _fanPct = 0;
static bool    _manualMode = false;
static uint8_t _manualPct  = 0;

// --- Linearna interpolacija temperaturne krivulje ---
// Vrne ciljni % (0–100) glede na temperaturo in settings.curveTemp/curvePct
static uint8_t interpolateCurve(float temp) {
    // Pod prvo točko → najnižji %
    if (temp <= settings.curveTemp[0])
        return settings.curvePct[0];

    // Nad zadnjo točko → polna hitrost
    if (temp >= settings.curveTemp[FAN_CURVE_POINTS - 1])
        return settings.curvePct[FAN_CURVE_POINTS - 1];

    // Poišči interval in linearna interpolacija
    for (int i = 0; i < FAN_CURVE_POINTS - 1; i++) {
        if (temp >= settings.curveTemp[i] && temp < settings.curveTemp[i + 1]) {
            float tSpan  = settings.curveTemp[i + 1] - settings.curveTemp[i];
            float pSpan  = (float)(settings.curvePct[i + 1] - settings.curvePct[i]);
            float ratio  = (temp - settings.curveTemp[i]) / tSpan;
            return (uint8_t)(settings.curvePct[i] + ratio * pSpan + 0.5f);
        }
    }
    return settings.curvePct[FAN_CURVE_POINTS - 1];
}

// --- Inicializacija ---
void initFan() {
    // IDF 5.x API: ledcAttach(pin, freq, resolution) — channel se dodeli avtomatsko
    ledcAttach(PIN_FAN_PWM, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    // Zaščita: ne izklopimo ventilatorja ob zagonu
    setFanPct(settings.fanMinPct);
    LOG_INFO("FAN", "Init OK — min %d%%", settings.fanMinPct);
}

// --- Direktna nastavitev hitrosti [0–100%] ---
void setFanPct(uint8_t pct) {
    // Upoštevaj konfigurirani minimum
    pct = max(pct, settings.fanMinPct);
    pct = constrain(pct, 0, 100);
    _fanPct = pct;

    uint8_t duty = (uint8_t)map(pct, 0, 100, FAN_PWM_MIN, FAN_PWM_MAX);
    ledcWrite(PIN_FAN_PWM, duty);

    portENTER_CRITICAL(&dataMux);
    sensorData.fanPct = _fanPct;
    portEXIT_CRITICAL(&dataMux);
}

uint8_t getFanPct() { return _fanPct; }

// --- DND (nočni tihi način) ---
bool isDndActive() {
    if (!settings.dndEnabled) return false;
    // Brez sinhroniziranega časa ne aktiviramo DND
    if (!timeSynced) return false;

    int h = (int)myTZ.hour();
    bool active;

    if (settings.dndFrom > settings.dndTo) {
        // Čez polnoč: npr. 22–07
        active = (h >= settings.dndFrom) || (h < settings.dndTo);
    } else {
        // Isti dan: npr. 08–20
        active = (h >= settings.dndFrom) && (h < settings.dndTo);
    }

    portENTER_CRITICAL(&dataMux);
    sensorData.dndActive = active;
    portEXIT_CRITICAL(&dataMux);

    return active;
}

// --- Posodobitev hitrosti ventilatorja (klic iz loop) ---
void updateFan() {
    // Ročni način — preskoči krivuljo in DND popolnoma
    if (_manualMode) {
        setFanPct(_manualPct);
        LOG_INFO("FAN", "ROCNO %d%%", _manualPct);
        return;
    }

    // Preberi temperaturo pod mutex zaščito
    float temp;
    portENTER_CRITICAL(&dataMux);
    temp = sensorData.temp;
    portEXIT_CRITICAL(&dataMux);

    // Senzor ne deluje → minimalna varnostna hitrost
    if (temp == ERR_FLOAT) {
        setFanPct(settings.fanMinPct);
        return;
    }

    // Izračunaj ciljni % iz temperaturne krivulje
    uint8_t pct = interpolateCurve(temp);

    // DND korekcija — omeji maksimum (samo v avtomatskem načinu)
    if (isDndActive()) {
        pct = min(pct, settings.dndMaxPct);
    }

    // Minimum
    pct = max(pct, settings.fanMinPct);

    setFanPct(pct);
    LOG_INFO("FAN", "T=%.1f C -> %d%% DND=%s",
             temp, pct, sensorData.dndActive ? "ON" : "off");
}

// --- Ročni način ---
void setManualMode(bool enabled, uint8_t pct) {
    _manualMode = enabled;
    _manualPct  = constrain(pct, 0, 100);

    portENTER_CRITICAL(&dataMux);
    sensorData.manualMode = enabled;
    sensorData.manualPct  = _manualPct;
    portEXIT_CRITICAL(&dataMux);

    if (enabled) {
        LOG_INFO("FAN", "Rocni nacin ON — %d%%", _manualPct);
    } else {
        LOG_INFO("FAN", "Rocni nacin OFF — vrni na avto");
    }
    updateFan();
}

bool isManualMode() {
    return _manualMode;
}

uint8_t getManualPct() {
    return _manualPct;
}
