// sensors.cpp — Implementacija branja SHT30 in INA219
#include "sensors.h"
#include "globals.h"
#include <Adafruit_SHT31.h>
#include <Adafruit_INA219.h>
#include <Wire.h>

static Adafruit_SHT31  sht30;
static Adafruit_INA219 ina219(ADDR_INA219);

bool initSensors() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    bool ok = true;

    if (!sht30.begin(ADDR_SHT30)) {
        Serial.println("[Sensors] SHT30 ni najden!");
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_SHT30;
        portEXIT_CRITICAL(&dataMux);
        ok = false;
    } else {
        Serial.println("[Sensors] SHT30 OK");
    }

    if (!ina219.begin()) {
        Serial.println("[Sensors] INA219 ni najden!");
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_INA219;
        portEXIT_CRITICAL(&dataMux);
        ok = false;
    } else {
        Serial.println("[Sensors] INA219 OK");
    }

    return ok;
}

void readSensors() {
    // --- SHT30: temperatura in vlažnost ---
    float t = sht30.readTemperature();
    float h = sht30.readHumidity();

    bool sht30ok = !isnan(t) && !isnan(h) && (t >= -20.0f) && (t <= 80.0f)
                                           && (h >=   0.0f) && (h <= 100.0f);

    portENTER_CRITICAL(&dataMux);
    if (sht30ok) {
        sensorData.temp = t;
        sensorData.hum  = h;
        sensorData.err &= ~ERR_SHT30;
    } else {
        // Prejšnja vrednost ostane, samo flag
        sensorData.err |= ERR_SHT30;
    }
    portEXIT_CRITICAL(&dataMux);

    if (sht30ok) {
        Serial.printf("[Sensors] SHT30: %.1f°C  %.1f%%\n", t, h);
    } else {
        Serial.println("[Sensors] SHT30: napaka branja");
    }

    // --- INA219: napetost, tok, moč ---
    float busV  = ina219.getBusVoltage_V();
    float currA = ina219.getCurrent_mA() / 1000.0f;
    float watt  = busV * currA;

    // Validacija: Mini PC 12V veja — razumna napetost 8–15V
    bool ina219ok = (busV >= 8.0f) && (busV <= 15.0f);

    portENTER_CRITICAL(&dataMux);
    if (ina219ok) {
        sensorData.volt = busV;
        sensorData.amp  = currA;
        sensorData.watt = watt;
        sensorData.err &= ~ERR_INA219;
    } else {
        // Prejšnje vrednosti ostanejo, samo flag
        sensorData.err |= ERR_INA219;
    }
    portEXIT_CRITICAL(&dataMux);

    if (ina219ok) {
        Serial.printf("[Sensors] INA219: %.2fV  %.3fA  %.2fW\n", busV, currA, watt);
    } else {
        Serial.printf("[Sensors] INA219: napaka (bus=%.2fV)\n", busV);
    }

    // Označi nova data — vedno, ne glede na napake posameznih senzorjev
    newSensorData = true;
}
