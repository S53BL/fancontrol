// fan_boost.cpp — Watt Feed-Forward Fan Boost
#include "fan_boost.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include <Preferences.h>
#include <math.h>
#include <Arduino.h>

void boostInit() {
    memset(&wattBoost, 0, sizeof(WattBoost));
    LOG_INFO("BOOST", "Init OK — boostPct=%d%%  thr=%.1fW  fadeMs=%lu  learnMs=%lu",
             settings.boostPct, settings.boostWattThreshold,
             (unsigned long)settings.boostEvalMs,
             (unsigned long)settings.boostLearnMs);
}

uint8_t boostGetExtra(float watt, float temp) {
    if (watt <= 0.0f || (sensorData.err & ERR_INA219)) {
        wattBoost.boostActive        = false;
        wattBoost.aboveThreshSinceMs = 0;
        wattBoost.fadeStartMs        = 0;
        return 0;
    }

    unsigned long now = millis();
    float thr = settings.boostWattThreshold;

    // ── 1. Detekcija praga ────────────────────────────────────────────────
    if (watt >= thr) {
        wattBoost.fadeStartMs = 0;

        if (wattBoost.aboveThreshSinceMs == 0) {
            wattBoost.aboveThreshSinceMs = now;
            LOG_INFO("BOOST", "W=%.1f presezel prag %.1fW — cakam %lus",
                     watt, thr, (unsigned long)(BOOST_ACTIVATE_MS / 1000));
        }

        if (!wattBoost.boostActive &&
            (now - wattBoost.aboveThreshSinceMs) >= BOOST_ACTIVATE_MS) {

            wattBoost.boostActive      = true;
            wattBoost.boostStartMs     = now;
            wattBoost.tempAtBoostStart = temp;
            wattBoost.evalDone         = false;
            LOG_INFO("BOOST", "AKTIVIRAN — boost=%d%%  T_start=%.1f",
                     settings.boostPct, temp);
        }

    } else {
        wattBoost.aboveThreshSinceMs = 0;

        if (wattBoost.boostActive) {
            if (wattBoost.fadeStartMs == 0) {
                wattBoost.fadeStartMs         = now;
                wattBoost.boostPctAtFadeStart = settings.boostPct;
                LOG_INFO("BOOST", "W pod pragom — zacenjam izzvenjanje (%lus)",
                         (unsigned long)(settings.boostEvalMs / 1000));
            }

            uint32_t elapsed = now - wattBoost.fadeStartMs;
            if (elapsed >= settings.boostEvalMs) {
                wattBoost.boostActive = false;
                wattBoost.fadeStartMs = 0;
                LOG_INFO("BOOST", "Izzvenjanje koncano");
                return 0;
            }

            float ratio = 1.0f - (float)elapsed / (float)settings.boostEvalMs;
            return (uint8_t)(wattBoost.boostPctAtFadeStart * ratio + 0.5f);
        }

        return 0;
    }

    // ── 2. Boost ni aktiven (še čakamo 5s) ───────────────────────────────
    if (!wattBoost.boostActive) return 0;

    // ── 3. Ocenjevanje po boostLearnMs ───────────────────────────────────
    if (!wattBoost.evalDone &&
        (now - wattBoost.boostStartMs) >= settings.boostLearnMs) {

        wattBoost.evalDone = true;

        float dT = temp - wattBoost.tempAtBoostStart;
        float dTperMin = dT / ((float)settings.boostLearnMs / 60000.0f);

        uint8_t oldPct = settings.boostPct;
        uint8_t newPct = oldPct;

        if (dTperMin < -BOOST_TEMP_DEADBAND) {
            float adj = oldPct - BOOST_EMA_ALPHA * fabsf(dTperMin) * 2.0f;
            newPct = (uint8_t)fmaxf(adj, (float)BOOST_PCT_MIN);
            LOG_INFO("BOOST", "Eval: HLAJENJE dT/min=%.2f — zmanjsujem %d->%d%%",
                     dTperMin, oldPct, newPct);
        } else if (dTperMin > BOOST_TEMP_DEADBAND) {
            float adj = oldPct + BOOST_EMA_ALPHA * dTperMin * 2.0f;
            newPct = (uint8_t)fminf(adj, (float)BOOST_PCT_MAX);
            LOG_INFO("BOOST", "Eval: GRETJE dT/min=%.2f — povecujem %d->%d%%",
                     dTperMin, oldPct, newPct);
        } else {
            LOG_INFO("BOOST", "Eval: RAVNOVESJE dT/min=%.2f — x ostane %d%%",
                     dTperMin, oldPct);
        }

        if (!settings.boostLocked && newPct != oldPct) {
            portENTER_CRITICAL(&dataMux);
            settings.boostPct = newPct;
            portEXIT_CRITICAL(&dataMux);
            saveSettings();
        }
    }

    // ── 4. Vrni aktivni boost% ────────────────────────────────────────────
    return settings.boostPct;
}

bool boostIsActive() {
    return wattBoost.boostActive;
}

uint8_t boostGetPct() {
    return settings.boostPct;
}
