// led.h — RGB LED status indikator
// Vgrajena RGB LED na TZT ESP32-S3-N16R8 (GPIO38, WS2812B kompatibilna)
// Barve označujejo stanje sistema med bootom in delovanjem

#pragma once
#include <Arduino.h>

// Inicializacija LED (klic iz setup() pred vsem)
void ledInit();

// Nastavi barvo (r, g, b — 0–255)
void ledSet(uint8_t r, uint8_t g, uint8_t b);

// Preddefinirane barve — boot faze
void ledBlue();    // Boot / init v teku
void ledYellow();  // WiFi connecting
void ledGreen();   // Vse OK — normalno delovanje
void ledRed();     // Kritična napaka
void ledOrange();  // Opozorilo (npr. samo delna napaka)
void ledOff();     // Izklopljeno
