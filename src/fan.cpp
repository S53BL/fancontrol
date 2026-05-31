// fan.cpp — Implementacija PWM krmiljenja ventilatorja
#include "fan.h"
#include "globals.h"
#include <Arduino.h>

static uint8_t _fanPct = 0;

void initFan() {
    // IDF 5.x API: ledcAttach(pin, freq, resolution) — channel se dodeli avtomatsko
    ledcAttach(PIN_FAN_PWM, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    setFanPct(0);
}

void setFanPct(uint8_t pct) {
    _fanPct = constrain(pct, 0, 100);
    uint8_t duty = (uint8_t)map(_fanPct, 0, 100, FAN_PWM_MIN, FAN_PWM_MAX);
    ledcWrite(PIN_FAN_PWM, duty);
    portENTER_CRITICAL(&dataMux);
    sensorData.fanPct = _fanPct;
    portEXIT_CRITICAL(&dataMux);
}

uint8_t getFanPct() { return _fanPct; }

bool isDndActive() {
    // TODO: preveri čas glede na settings.dndFrom/To
    return false;
}

void updateFan() {
    // TODO: preračun po temperaturni krivulji + DND logika
}
