// fan.cpp — Implementacija PWM krmiljenja ventilatorja
#include "fan.h"
#include "globals.h"
#include "logging.h"
#include "fan_adapt.h"
#include "fan_boost.h"
#include <Arduino.h>

static uint8_t  _fanPct     = 0;
static bool     _manualMode = false;
static uint8_t  _manualPct  = 0;

// Hysteresis state
static bool     _fanRunning  = false;
static bool     _kickActive  = false;
static uint32_t _kickStartMs = 0;

// --- Linearna interpolacija temperaturne krivulje ---
// Vrne ciljni % (0–100) glede na temperaturo in settings.curveTemp/curvePct
static uint8_t interpolateCurve(float temp) {
    if (temp <= settings.curveTemp[0])
        return settings.curvePct[0];
    if (temp >= settings.curveTemp[FAN_CURVE_POINTS - 1])
        return settings.curvePct[FAN_CURVE_POINTS - 1];
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

// Konverzija %user → %PWM (identična logiki v setFanPct())
static uint8_t _userToPwm(uint8_t userPct) {
    if (userPct == 0) return 0;
    float pwm = 27.0f + (float)(userPct - 1) * 73.0f / 99.0f;
    return (uint8_t)(pwm + 0.5f);
}

// Konverzija %PWM → %user (inverzna funkcija)
static uint8_t _pwmToUser(uint8_t pwmPct) {
    if (pwmPct == 0) return 0;
    if (pwmPct < 27) return 1;
    float user = 1.0f + (float)((int)pwmPct - 27) * 99.0f / 73.0f;
    return (uint8_t)(fminf(100.0f, user) + 0.5f);
}

// applyHysteresisAndKick — hysteresis logika za krmiljenje motorja
// requestedPct: %user (regulacijska logika)
// fanStartPct, fanStopPct: %PWM (fizikalni parametri motorja)
// return: %user (posreduje setFanPct() ki pretvori v PWM)
static uint8_t applyHysteresisAndKick(uint8_t requestedPct) {
    uint8_t requestedPwm = _userToPwm(requestedPct);
    uint32_t now = millis();

    if (_kickActive) {
        if (now - _kickStartMs >= FAN_KICK_MS) {
            _kickActive = false;
            if (requestedPct == 0 || requestedPwm < settings.fanStopPct) {
                _fanRunning = false;
                LOG_INFO("FAN", "Kick koncan — req=%d%% (%d%%PWM) pod stop=%d%%PWM → OFF",
                         requestedPct, requestedPwm, settings.fanStopPct);
                return 0;
            }
            LOG_INFO("FAN", "Kick koncan → %d%%user", requestedPct);
            return requestedPct;
        }
        return _pwmToUser(settings.fanStartPct);
    }

    if (!_fanRunning) {
        if (requestedPct > 0 && requestedPwm >= settings.fanStartPct) {
            _fanRunning  = true;
            _kickActive  = true;
            _kickStartMs = now;
            LOG_INFO("FAN", "Startup kick: req=%d%%user (%d%%PWM) kick=%d%%PWM (%lums)",
                     requestedPct, requestedPwm,
                     settings.fanStartPct, (unsigned long)FAN_KICK_MS);
            return _pwmToUser(settings.fanStartPct);
        }
        return 0;
    }

    // Fan teče — preveri ali PWM ostane nad fizikalnim minimumom
    if (requestedPct == 0 || requestedPwm < settings.fanStopPct) {
        _fanRunning = false;
        LOG_INFO("FAN", "OFF: req=%d%%user (%d%%PWM) pod stop=%d%%PWM",
                 requestedPct, requestedPwm, settings.fanStopPct);
        return 0;
    }
    return requestedPct;
}

// --- Inicializacija ---
void initFan() {
    uint32_t freq = (settings.fanPwmFreq >= 10 && settings.fanPwmFreq <= 50000)
                    ? settings.fanPwmFreq : FAN_PWM_FREQ;
    ledcAttach(PIN_FAN_PWM, freq, FAN_PWM_RESOLUTION);

    _fanRunning  = false;
    _kickActive  = false;
    _kickStartMs = 0;

    setFanPct(0);
    LOG_INFO("FAN", "Init OK — start=%d%%PWM stop=%d%%PWM kick=%lums",
             settings.fanStartPct, settings.fanStopPct, (unsigned long)FAN_KICK_MS);
}

// --- Direktna nastavitev hitrosti [user 0–100%] → PWM ---
// 0% user = 0% PWM (ugasnjen)
// 1–100% user = 27–100% PWM (linearna preslikava na fizični razpon)
void setFanPct(uint8_t pct) {
    pct = constrain(pct, 0, 100);
    _fanPct = pct;

    uint8_t duty;
    if (pct == 0) {
        duty = 0;
    } else {
        float pwmPct = 27.0f + (float)(pct - 1) * 73.0f / 99.0f;
        if (settings.fanPwmInvert) {
            duty = (uint8_t)map((long)pwmPct, 0, 100, FAN_PWM_MAX, FAN_PWM_MIN);
        } else {
            duty = (uint8_t)(pwmPct / 100.0f * FAN_PWM_MAX + 0.5f);
        }
    }
    ledcWrite(PIN_FAN_PWM, duty);

    portENTER_CRITICAL(&dataMux);
    sensorData.fanPct = _fanPct;
    portEXIT_CRITICAL(&dataMux);
}

uint8_t getFanPct() { return _fanPct; }

// --- Reinit PWM ob spremembi frekvence ali invert (brez reseta hysteresis state) ---
void reinitFanPwm() {
    uint32_t freq = (settings.fanPwmFreq >= 10 && settings.fanPwmFreq <= 50000)
                    ? settings.fanPwmFreq : FAN_PWM_FREQ;
    ledcDetach(PIN_FAN_PWM);
    ledcAttach(PIN_FAN_PWM, freq, FAN_PWM_RESOLUTION);
    setFanPct(_fanPct);
    LOG_INFO("FAN", "PWM reinit — freq=%lu Hz  invert=%s",
             (unsigned long)freq, settings.fanPwmInvert ? "ON" : "off");
}

// --- DND (nočni tihi način) ---
bool isDndActive() {
    if (!settings.dndEnabled) return false;
    if (!timeSynced) return false;

    int h = (int)myTZ.hour();
    bool active;

    if (settings.dndFrom > settings.dndTo) {
        active = (h >= settings.dndFrom) || (h < settings.dndTo);
    } else {
        active = (h >= settings.dndFrom) && (h < settings.dndTo);
    }

    portENTER_CRITICAL(&dataMux);
    sensorData.dndActive = active;
    portEXIT_CRITICAL(&dataMux);

    return active;
}

// --- Posodobitev hitrosti ventilatorja (klic iz loop) ---
void updateFan() {
    if (_manualMode) {
        uint8_t actualPct = applyHysteresisAndKick(_manualPct);
        setFanPct(actualPct);
        LOG_INFO("FAN", "ROCNO req=%d%% actual=%d%% running=%s",
                 _manualPct, actualPct, _fanRunning ? "ON" : "OFF");
        return;
    }

    float temp;
    portENTER_CRITICAL(&dataMux);
    temp = sensorData.temp;
    portEXIT_CRITICAL(&dataMux);

    if (temp == ERR_FLOAT) {
        // Senzor ne deluje → varnostna vrednost: fanStartPct%PWM → pretvorba v %user
        uint8_t actualPct = applyHysteresisAndKick(_pwmToUser(settings.fanStartPct));
        setFanPct(actualPct);
        return;
    }

    uint8_t pct = interpolateCurve(temp);

    if (isDndActive()) {
        pct = min(pct, settings.dndMaxPct);
    }

    float wattNow;
    portENTER_CRITICAL(&dataMux);
    wattNow = sensorData.watt;
    portEXIT_CRITICAL(&dataMux);

    uint8_t extra = boostGetExtra(wattNow, temp);
    if (extra > 0) {
        uint8_t boosted = (uint8_t)constrain((int)pct + (int)extra, 0, 100);
        if (isDndActive()) boosted = min(boosted, settings.dndMaxPct);
        pct = boosted;
    }

    uint8_t actualPct = applyHysteresisAndKick(pct);
    setFanPct(actualPct);
    LOG_INFO("FAN", "T=%.1f C -> req=%d%% actual=%d%% boost=%d%% running=%s DND=%s",
             temp, pct, actualPct, extra,
             _fanRunning ? "ON" : "OFF",
             sensorData.dndActive ? "ON" : "off");

    adaptUpdate(temp, pct);
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
        uint8_t actualPct = applyHysteresisAndKick(_manualPct);
        setFanPct(actualPct);
    } else {
        LOG_INFO("FAN", "Rocni nacin OFF — vrni na avto");
        updateFan();
    }
}

bool isManualMode() {
    return _manualMode;
}

uint8_t getManualPct() {
    return _manualPct;
}
