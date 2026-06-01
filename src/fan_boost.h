// fan_boost.h — Watt Feed-Forward Fan Boost
// Zazna povečano porabo in takoj dvigne fan% za naučeni x%.
// Po ocenjevalnem času primerja trend temperature in prilagodi x%.
// Ko W pade pod prag → boost postopoma izzveni.
#pragma once
#include <stdint.h>

void    boostInit();
uint8_t boostGetExtra(float watt, float temp);
bool    boostIsActive();
uint8_t boostGetPct();
