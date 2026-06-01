// fan_adapt.h — Adaptivni fan control (samo-učeča krivulja)
// Princip: opazuje termalno ravnovesje, počasi posodablja curvePct[]
// Zaklenjena točka (curveLocked[i]==true) se nikoli ne posodablja.
#pragma once
#include <stdint.h>

void adaptInit();
void adaptUpdate(float temp, uint8_t fanPct);
int  adaptGetZone(float temp);
bool adaptIsActive(int i);
void adaptReset();
void adaptSaveNVS();
void adaptLoadNVS();
