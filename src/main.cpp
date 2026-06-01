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
#include "monitor.h"
#include "fan_adapt.h"
#include <WiFi.h>
#include <ezTime.h>
#include "led.h"
#include "suntime.h"

void setup() {
    // 1. Serial — mora biti prvi, da vidimo LED diagnostiko pri bootanju
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println("\n\n=== fancontrol boot ===");
    Serial.printf("FW: %s | Flash: %dMB | PSRAM: %dMB\n",
                  FW_VERSION,
                  ESP.getFlashChipSize() / (1024*1024),
                  ESP.getPsramSize()     / (1024*1024));

    // 2. LED (modra = boot v teku)
    Serial.printf("[LED] Init na GPIO%d\n", PIN_RGB_LED);
    ledInit();
    ledBlue();
    Serial.println("[LED] Blue OK");

    // 3. Logging (mora biti pred vsem kar logira)
    logInit();
    LOG_INFO("BOOT", "Logging init OK");

    // 4. Globals + NVS
    initGlobals();
    LOG_INFO("BOOT", "Globals OK");

    // 5. Graf buffer (PSRAM)
    if (!psramFound()) {
        LOG_WARN("BOOT", "PSRAM ni dostopen — graf buffer bo na heap");
    }
    graphStoreInit();
    LOG_INFO("BOOT", "Graph store OK (%d entries)", GRAPH_BUFFER_SIZE);

    // 6. Ventilator — PWM init (ne more failati)
    initFan();
    LOG_INFO("BOOT", "Fan PWM init OK");
    adaptInit();
    LOG_INFO("BOOT", "Adapt fan init OK");

    // 6b. Monitor init
    monitorInit();
    LOG_INFO("BOOT", "Monitor init OK");

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
    if (!initDisplay()) {
        LOG_WARN("BOOT", "ePaper: ni priključen ali BUSY timeout — nadaljujem brez zaslona");
    } else {
        LOG_INFO("BOOT", "ePaper OK");
        showBootScreen();
        LOG_INFO("BOOT", "Boot screen prikazan");
    }

    // 9. WiFi + NTP + Web (LED rumena med povezovanjem)
    ledYellow();
    initWebserver();

    // 10. Weather — eksplicitno ob zagonu (ne čakamo prve iteracije loopa)
    LOG_INFO("BOOT", "Pridobivam vremenske podatke...");
    fetchWeather();
    lastWeatherFetchMs = millis();  // prepreči ponovni fetch na prvi loop iteraciji

    // 11. Prvo branje senzorjev + izračun ventilatorja
    // delay(50) — SHT30 potrebuje ~15ms za prvo meritev po Wire.begin()
    LOG_INFO("BOOT", "Prvo branje senzorjev...");
    delay(50);
    readSensors();
    updateFan();
    LOG_INFO("BOOT", "Senzorji in fan OK — fan=%d%%  err=0x%02X",
             sensorData.fanPct, sensorData.err);

    // 12. Končni LED status
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

    // 13. Kratek delay — WiFi/NTP/Weather stack se ustali
    if (!(sensorData.err & ERR_DISPLAY)) {
        LOG_INFO("BOOT", "Cakam 7s pred prvim display refresh...");
        delay(7000);

        // Sunrise/sunset — lokalni izračun (pred prvim prikazom)
        if (timeSynced) {
            int utcOff = getCETOffset(myTZ.day(), myTZ.month(), myTZ.year());
            calcSunTimes(myTZ.day(), myTZ.month(), myTZ.year(),
                         atof(WEATHER_LAT), atof(WEATHER_LON),
                         utcOff,
                         weatherData.sunrise, weatherData.sunset);
            LOG_INFO("SUN", "Izracun: vzh=%s  zah=%s  UTC+%d",
                     weatherData.sunrise, weatherData.sunset, utcOff);
        }

        updateDisplay();
        lastDisplayRefreshMs = millis();
        LOG_INFO("BOOT", "Prvi display refresh OK");
    }
}

void loop() {
    unsigned long now = millis();
    static uint8_t lastSunDay = 0;

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
        monitorRun();

        // Peak temp tracker
        if (sensorData.temp > ERR_FLOAT + 1.0f && sensorData.temp > peakTemp)
            peakTemp = sensorData.temp;
    }

    // Dnevni recalc sunrise/sunset (enkrat na dan ob spremembi datuma)
    if (timeSynced) {
        uint8_t todayDay = (uint8_t)myTZ.day();
        if (todayDay != lastSunDay) {
            lastSunDay = todayDay;
            int utcOff = getCETOffset(myTZ.day(), myTZ.month(), myTZ.year());
            calcSunTimes(myTZ.day(), myTZ.month(), myTZ.year(),
                         atof(WEATHER_LAT), atof(WEATHER_LON),
                         utcOff,
                         weatherData.sunrise, weatherData.sunset);
            LOG_INFO("SUN", "Dnevni recalc: vzh=%s  zah=%s  UTC+%d",
                     weatherData.sunrise, weatherData.sunset, utcOff);
        }
    }

    // ePaper osvežitev
    if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL) {
        lastDisplayRefreshMs = now;
        updateDisplay();
    }

    // Weather fetch — vsakih 30 minut (Core 0 WiFi stack)
    if (millis() - lastWeatherFetchMs >= WEATHER_FETCH_INTERVAL || lastWeatherFetchMs == 0) {
        lastWeatherFetchMs = millis();
        fetchWeather();
    }

    handleWebserver(); // mDNS + NTP re-sync + WiFi watchdog
    delay(10);
}
