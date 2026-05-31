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
    // TODO: branje temp/hum iz SHT30, V/A/W iz INA219
    // Zapiši v sensorData z mutex zaščito
}
