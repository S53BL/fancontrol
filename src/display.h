// display.h — GxEPD2 ePaper WeAct 2.9" B/W
#pragma once
#include <Arduino.h>

bool initDisplay();    // GxEPD2 init, SPI, U8g2 init, bel zaslon; false = ni priključen/BUSY timeout
void showBootScreen(); // Zagonski ekran z QR kodo, IP, mDNS, monitor IP
// fullRefresh=true  → setFullWindow (počisti ghosting, z inverznim flashom)
// fullRefresh=false → setPartialWindow cel zaslon (brez flasha, privzeto)
void updateDisplay(bool fullRefresh = false);

// Vremenske ikone (adaptirane iz G6EJD/LilyGo projekta, scale=WX_ICON_SCALE)
void wxDrawConditions(int x, int y, uint8_t wxCode, bool isNight);
void wxDrawConditionsScaled(int x, int y, uint8_t wxCode, bool isNight, int sc, int ls);
static void wxAddCloud(int x, int y, int scale, int linesize);
static void wxAddSun(int x, int y, int scale);
static void wxAddMoon(int x, int y);
static void wxAddRain(int x, int y, int scale);
static void wxAddSnow(int x, int y, int scale);
static void wxAddTstorm(int x, int y, int scale);
static void wxAddFog(int x, int y, int scale);
