// main.cpp — fancontrol glavna zanka
// Plošča: TZT ESP32-S3-N16R8
// Core 0: WiFi stack + AsyncWebServer
// Core 1: senzorji, PWM fan, ePaper (loop)

#include <Arduino.h>
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "sensors.h"
#include "fan.h"
#include "display.h"
#include "webserver.h"
#include "graph_store.h"
#include <WiFi.h>
#include <ezTime.h>
#include "led.h"

void setup() {
    // 1. LED — takoj na začetku (modra = boot v teku)
    ledInit();
    ledBlue();

    // 2. Serial — z zamudo da monitor uspe prikazati začetek
    Serial.begin(SERIAL_BAUD);
    delay(BOOT_SERIAL_DELAY_MS);
    Serial.println("\n\n=== fancontrol boot ===");
    Serial.printf("FW: %s | Flash: %dMB | PSRAM: %dMB\n",
                  FW_VERSION,
                  ESP.getFlashChipSize() / (1024*1024),
                  ESP.getPsramSize()     / (1024*1024));

    // 3. Logging (mora biti pred vsem kar logira)
    logInit();
    LOG_INFO("BOOT", "Logging init OK");

    // 4. Globals + NVS
    initGlobals();
    LOG_INFO("BOOT", "Globals OK");

    // 5. Graf buffer (PSRAM)
    graphStoreInit();
    LOG_INFO("BOOT", "Graph store OK (%d entries)", GRAPH_BUFFER_SIZE);

    // 6. Ventilator — PWM init (ne more failati)
    initFan();
    LOG_INFO("BOOT", "Fan PWM init OK");

    // 7. Senzorji — z zamudo za stabilizacijo napajanja
    delay(BOOT_SENSOR_DELAY_MS);
    initSensors();
    if (sensorData.err & (ERR_SHT30 | ERR_INA219)) {
        ledOrange(); // opozorilo — delna napaka
        LOG_WARN("BOOT", "Senzorji: ERR=0x%02X (delovanje z omejitvami)", sensorData.err);
    } else {
        LOG_INFO("BOOT", "Senzorji OK");
    }

    // 8. ePaper zaslon
    initDisplay();
    if (sensorData.err & ERR_DISPLAY) {
        LOG_WARN("BOOT", "ePaper: ni priključen ali napaka");
    } else {
        LOG_INFO("BOOT", "ePaper OK");
    }

    // 9. WiFi + NTP + Web (LED rumena med povezovanjem)
    ledYellow();
    initWebserver();

    // 10. Končni LED status
    if (sensorData.err == ERR_NONE) {
        ledGreen();
        LOG_INFO("BOOT", "Boot complete — vse OK");
    } else if (sensorData.err & ERR_WIFI) {
        ledRed();
        LOG_ERROR("BOOT", "Boot complete — WIFI napaka (ERR=0x%02X)", sensorData.err);
    } else {
        ledOrange();
        LOG_WARN("BOOT", "Boot complete z opozorili (ERR=0x%02X)", sensorData.err);
    }

    Serial.println("=== Boot complete ===");
}

void loop() {
    unsigned long now = millis();

    // Branje senzorjev
    if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL) {
        lastSensorReadMs = now;
        readSensors();
        updateFan();
        // Posodobi LED status glede na trenutne napake
        if (sensorData.err == ERR_NONE) {
            ledGreen();
        } else if (sensorData.err & ERR_WIFI) {
            ledRed();
        } else {
            ledOrange();
        }
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

        // Peak tracker
        if (sensorData.temp > ERR_FLOAT + 1.0f && sensorData.temp > peakTemp)
            peakTemp = sensorData.temp;
        if (sensorData.watt > peakWatt)
            peakWatt = sensorData.watt;
    }

    // ePaper osvežitev
    if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL) {
        lastDisplayRefreshMs = now;
        updateDisplay();
    }

    handleWebserver(); // mDNS + NTP re-sync + WiFi watchdog
    delay(10);
}
