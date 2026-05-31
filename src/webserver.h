// webserver.h — AsyncWebServer za fancontrol
#pragma once

void initWebserver();   // WiFi connect + server.begin() + registracija endpointov
void handleWebserver(); // Klic v loop() za mDNS
void fetchWeather();    // Fetch vremenskih podatkov z Open-Meteo API
