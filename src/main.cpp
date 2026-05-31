// main.cpp — fancontrol glavna zanka
// Plošča: TZT ESP32-S3-N16R8
// Core 0: WiFi stack + AsyncWebServer
// Core 1: senzorji, PWM fan, ePaper (loop)

#include <Arduino.h>
#include "config.h"
#include "globals.h"
#include "sensors.h"
#include "fan.h"
#include "display.h"
#include "webserver.h"
#include "graph_store.h"
#include <WiFi.h>
#include <ezTime.h>

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println("\n=== fancontrol boot ===");

    initGlobals();       // NVS settings + sensorData init
    graphStoreInit();    // PSRAM buffer
    initFan();           // PWM init — ventilator na 0%
    initSensors();       // I2C + SHT30 + INA219
    initDisplay();       // ePaper init
    initWebserver();     // WiFi + NTP + AsyncWebServer

    Serial.println("=== Boot complete ===");
}

void loop() {
    unsigned long now = millis();

    // Branje senzorjev
    if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL) {
        lastSensorReadMs = now;
        readSensors();
        updateFan();
        newSensorData = true;
    }

    // Shranjevanje v graf buffer
    if (newSensorData && (now - lastGraphStoreMs >= GRAPH_STORE_INTERVAL)) {
        lastGraphStoreMs = now;
        newSensorData = false;
        GraphPoint pt;
        portENTER_CRITICAL(&dataMux);
        pt.ts     = (uint32_t)time(nullptr);
        pt.temp   = sensorData.temp;
        pt.hum    = sensorData.hum;
        pt.volt   = sensorData.volt;
        pt.watt   = sensorData.watt;
        pt.fanPct = sensorData.fanPct;
        portEXIT_CRITICAL(&dataMux);
        graphAddPoint(pt);
    }

    // ePaper osvežitev
    if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL) {
        lastDisplayRefreshMs = now;
        updateDisplay();
    }

    // WiFi watchdog
    if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = now;
        // TODO: reconnect če ni WiFi
    }

    handleWebserver(); // mDNS
    delay(10);
}
