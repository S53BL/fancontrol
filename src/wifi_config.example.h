// wifi_config.example.h — VZOREC za wifi_config.h
// 1. Kopiraj to datoteko v src/wifi_config.h
// 2. Vnesi svoje WiFi podatke
// 3. wifi_config.h je v .gitignore — nikoli ne bo naložena v git
#pragma once

// Seznam WiFi omrežij — poskuša po vrsti dokler se ne poveže
static const char* WIFI_SSID_LIST[]   = {"Omrezje1", "Omrezje2"};
static const char* WIFI_PASS_LIST[]   = {"Geslo1",   "Geslo2"};
static const int   WIFI_NETWORK_COUNT = 2;

// --- Statični IP (ESP32) ---
static const char* WIFI_STATIC_IP     = "192.168.x.x";
static const char* WIFI_STATIC_GW     = "192.168.x.1";
static const char* WIFI_STATIC_SUBNET = "255.255.255.0";
static const char* WIFI_STATIC_DNS    = "8.8.8.8";

// --- NTP strežniki (po vrsti, prvi dostopen se uporabi) ---
static const char* NTP_SERVER_LIST[] = {
    "ntp1.arnes.si",
    "ntp2.arnes.si",
    "0.pool.ntp.org",
    "1.pool.ntp.org",
    "time.google.com"
};
static const int NTP_SERVER_COUNT = 5;
