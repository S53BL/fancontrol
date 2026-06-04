// webserver_about.h — /about stran za fancontrol
// Ločena datoteka ker je vsebina obsežna in bi webserver.cpp postal
// neobvladljiv. Servirano kot inline C++ string (ne gzip) ker je stran
// statična in se redko kliče.
#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Registrira /about handler na obstoječi server instanci.
// Kliči iz initWebserver() PRED server.begin().
void registerAboutHandler(AsyncWebServer& server);
