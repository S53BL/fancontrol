// webserver.h — AsyncWebServer za fancontrol
#pragma once
#include <Arduino.h>

void initWebserver();       // WiFi connect + server.begin() + registracija endpointov
void handleWebserver();     // Klic v loop() za mDNS + NTP + WiFi watchdog + roaming
void fetchWeather();        // Fetch vremenskih podatkov z Open-Meteo API
void wifiScanHistoryAdd(uint8_t type, const char* ssid, const char* bssid,
                        int rssiPre, int rssiPost); // Doda vpis v WiFi scan history
