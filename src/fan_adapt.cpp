// fan_adapt.cpp — Adaptivni fan control
#include "fan_adapt.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include <Preferences.h>
#include <math.h>

static const float   _defTemp[FAN_CURVE_POINTS] = {
    FAN_CURVE_TEMP_0, FAN_CURVE_TEMP_1, FAN_CURVE_TEMP_2,
    FAN_CURVE_TEMP_3, FAN_CURVE_TEMP_4, FAN_CURVE_TEMP_5
};
static const uint8_t _defPct[FAN_CURVE_POINTS] = {
    FAN_CURVE_PCT_0, FAN_CURVE_PCT_1, FAN_CURVE_PCT_2,
    FAN_CURVE_PCT_3, FAN_CURVE_PCT_4, FAN_CURVE_PCT_5
};

static const char* _nvsConf[FAN_CURVE_POINTS]    = {"conf0","conf1","conf2","conf3","conf4","conf5"};
static const char* _nvsPct[FAN_CURVE_POINTS]     = {"pct0","pct1","pct2","pct3","pct4","pct5"};

void adaptInit() {
    memset(&adaptObserver, 0, sizeof(AdaptObserver));
    adaptLoadNVS();
    LOG_INFO("ADAPT", "Init OK — confidence: %d %d %d %d %d %d",
             settings.curveConfidence[0], settings.curveConfidence[1],
             settings.curveConfidence[2], settings.curveConfidence[3],
             settings.curveConfidence[4], settings.curveConfidence[5]);
}

int adaptGetZone(float temp) {
    for (int i = 0; i < FAN_CURVE_POINTS - 1; i++) {
        if (temp >= settings.curveTemp[i] && temp < settings.curveTemp[i + 1])
            return i;
    }
    if (temp >= settings.curveTemp[FAN_CURVE_POINTS - 1])
        return FAN_CURVE_POINTS - 1;
    return -1;
}

bool adaptIsActive(int i) {
    if (i < 0 || i >= FAN_CURVE_POINTS) return false;
    return settings.curveConfidence[i] >= ADAPT_CONFIDENCE_THRESH;
}

void adaptUpdate(float temp, uint8_t fanPct) {
    if (temp == ERR_FLOAT) return;
    if (sensorData.manualMode) return;

    AdaptObserver& obs = adaptObserver;

    obs.tempHistory[obs.histIdx] = temp;
    obs.fanHistory[obs.histIdx]  = fanPct;
    obs.histIdx = (obs.histIdx + 1) % ADAPT_EQUILIBRIUM_WINDOW;
    if (obs.histCount < ADAPT_EQUILIBRIUM_WINDOW) obs.histCount++;

    if (obs.histCount < ADAPT_EQUILIBRIUM_WINDOW) return;

    float tMin = obs.tempHistory[0], tMax = obs.tempHistory[0];
    bool fanStable  = true;
    uint8_t firstFan = obs.fanHistory[0];

    for (uint8_t i = 1; i < ADAPT_EQUILIBRIUM_WINDOW; i++) {
        float t = obs.tempHistory[i];
        if (t < tMin) tMin = t;
        if (t > tMax) tMax = t;
        if (obs.fanHistory[i] != firstFan) fanStable = false;
    }

    bool inEquilibrium = ((tMax - tMin) <= ADAPT_TEMP_STABILITY) && fanStable;

    unsigned long now = millis();

    if (!inEquilibrium) {
        obs.equilibriumActive  = false;
        obs.equilibriumStartMs = 0;
        return;
    }

    if (!obs.equilibriumActive) {
        obs.equilibriumActive  = true;
        obs.equilibriumStartMs = now;
        LOG_INFO("ADAPT", "Ravnovesje zaceto: T=%.1f fan=%d%%", temp, fanPct);
        return;
    }

    if (now - obs.equilibriumStartMs < ADAPT_MIN_EQUILIBRIUM_MS) return;
    if (obs.lastUpdateMs > 0 && now - obs.lastUpdateMs < ADAPT_MIN_EQUILIBRIUM_MS) return;

    float tSum = 0;
    for (uint8_t i = 0; i < ADAPT_EQUILIBRIUM_WINDOW; i++) tSum += obs.tempHistory[i];
    float tAvg = tSum / ADAPT_EQUILIBRIUM_WINDOW;

    int zone = adaptGetZone(tAvg);
    if (zone < 0) return;

    if (settings.curveLocked[zone]) {
        LOG_INFO("ADAPT", "Cona %d zaklenjena — preskocim", zone);
        return;
    }

    float oldPct = (float)settings.curvePct[zone];
    float newPct = oldPct + ADAPT_EMA_ALPHA * ((float)fanPct - oldPct);
    newPct = fmaxf(newPct, (float)settings.fanMinPct);
    newPct = fminf(newPct, 100.0f);
    uint8_t newPctInt = (uint8_t)(newPct + 0.5f);

    uint8_t oldConf = settings.curveConfidence[zone];
    uint8_t newConf = (oldConf < ADAPT_CONFIDENCE_MAX) ? oldConf + 1 : ADAPT_CONFIDENCE_MAX;

    portENTER_CRITICAL(&dataMux);
    settings.curvePct[zone]        = newPctInt;
    settings.curveConfidence[zone] = newConf;
    portEXIT_CRITICAL(&dataMux);

    obs.lastUpdateMs       = now;
    obs.equilibriumStartMs = now;

    LOG_INFO("ADAPT", "Cona %d posodobljena: pct %d->%d  conf %d->%d  T=%.1f",
             zone, (int)oldPct, (int)newPctInt, (int)oldConf, (int)newConf, tAvg);

    adaptSaveNVS();
    saveSettings();
}

void adaptReset() {
    portENTER_CRITICAL(&dataMux);
    for (int i = 0; i < FAN_CURVE_POINTS; i++) {
        settings.curvePct[i]        = _defPct[i];
        settings.curveConfidence[i] = 0;
    }
    portEXIT_CRITICAL(&dataMux);

    Preferences prefs;
    prefs.begin(ADAPT_NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();

    memset(&adaptObserver, 0, sizeof(AdaptObserver));
    saveSettings();
    LOG_INFO("ADAPT", "Reset ucenja — vrnjeno na default vrednosti");
}

void adaptSaveNVS() {
    Preferences prefs;
    prefs.begin(ADAPT_NVS_NAMESPACE, false);
    for (int i = 0; i < FAN_CURVE_POINTS; i++) {
        prefs.putUChar(_nvsConf[i], settings.curveConfidence[i]);
        prefs.putUChar(_nvsPct[i],  settings.curvePct[i]);
    }
    prefs.end();
}

void adaptLoadNVS() {
    Preferences prefs;
    prefs.begin(ADAPT_NVS_NAMESPACE, true);
    for (int i = 0; i < FAN_CURVE_POINTS; i++) {
        uint8_t conf = prefs.getUChar(_nvsConf[i], 0);
        uint8_t pct  = prefs.getUChar(_nvsPct[i], _defPct[i]);
        settings.curveConfidence[i] = conf;
        if (conf > 0) settings.curvePct[i] = pct;
    }
    prefs.end();
}
