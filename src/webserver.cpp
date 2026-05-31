// webserver.cpp — Web vmesnik za fancontrol
// Arhitektura: HTML vgrajen v firmware (buildPage()), brez LittleFS
// Vzorec: VAC_Plug (buildPage, /save POST, Preferences) + vent_SEW (API endpointi)
#include "webserver.h"
#include "globals.h"
#include "config.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static AsyncWebServer server(80);

static String buildPage() {
    // TODO: celoten HTML z Chart.js grafi, 3 tabi (Dashboard, Nastavitve, Sistem)
    return "<html><body><h1>fancontrol — v0.1</h1><p>TODO</p></body></html>";
}

void initWebserver() {
    // TODO: WiFi.begin(), čakaj na connect, nastavi mDNS
    // Endpointi:
    //   GET  /           → buildPage()
    //   GET  /api/data   → JSON trenutnih vrednosti
    //   GET  /api/history → JSON array za grafe
    //   POST /save       → shrani nastavitve v NVS
    //   GET  /update     → OTA HTML
    //   POST /update     → OTA flash
    server.begin();
}

void handleWebserver() {
    // mDNS update — klic v loop()
}
