// fan.h — PWM krmiljenje ventilatorja
#pragma once
#include "config.h"

void initFan();              // Inicializacija LEDC PWM
void updateFan();            // Izračun in nastavitev hitrosti glede na temp + DND
void setFanPct(uint8_t pct); // Direktna nastavitev [0–100%]
uint8_t getFanPct();         // Trenutna hitrost [%]
bool isDndActive();          // Je DND aktiven glede na čas

void reinitFanPwm();

// --- Ročni način ---
void    setManualMode(bool enabled, uint8_t pct);
bool    isManualMode();
uint8_t getManualPct();
