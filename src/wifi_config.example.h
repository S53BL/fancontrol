// wifi_config.example.h — VZOREC za wifi_config.h
// 1. Kopiraj to datoteko v src/wifi_config.h
// 2. Vnesi svoje WiFi podatke
// 3. wifi_config.h je v .gitignore — nikoli ne bo naložena v git
#pragma once

// Seznam WiFi omrežij — poskuša po vrsti dokler se ne poveže
const char* WIFI_SSID_LIST[]   = {"Omrezje1", "Omrezje2"};
const char* WIFI_PASS_LIST[]   = {"Geslo1",   "Geslo2"};
const int   WIFI_NETWORK_COUNT = 2;
