// nanopi_client.h — HTTP klient za NanoPi R3S /api/fandata endpoint
#pragma once
#include <Arduino.h>

// Podatki prejeti od NanoPi (zadnji uspešni fetch)
struct NanoPiData {
    uint32_t ts;                // Unix timestamp odgovora
    float    networkCurrentMbits; // Trenutni promet [Mbit/s]
    float    networkPeakMbits;    // Peak promet [Mbit/s]
    uint8_t  networkBarPct;       // Bar % (ni omejen na 100)
    uint32_t attacksCurrentPh;    // Blokade banIP v zadnji uri
    uint32_t attacksPeakPh;       // Peak blokad/h
    uint8_t  attacksBarPct;       // Bar % (ni omejen na 100)
    bool     banipRunning;        // Ali banIP teče
    uint32_t banipLastReload;     // Timestamp zadnjega banIP reload
    bool     serverOk;            // NanoPi lastni zaključek o dosegljivosti Siriusa
    bool     stale;               // true = podatki starejši od NANOPI_STALE_THRESHOLD_MS
    bool     valid;               // false = še ni bilo uspešnega fetcha
    uint8_t  failCount;           // Zaporednih napak
};

void         nanopiClientInit();
void         nanopiClientLoop();   // klic iz loop() v main.cpp
NanoPiData   nanopiGetData();      // thread-safe branje zadnjih podatkov
