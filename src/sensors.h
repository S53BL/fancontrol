// sensors.h — SHT30 in INA219 branje
#pragma once
#include "config.h"

bool initSensors();    // Inicializacija I2C, SHT30, INA219 + initPeakWatt()
void initPeakWatt();   // Naloži peak watt iz NVS; kliče se ob zagonu (brez decay — ni NTP)
void readSensors();    // Branje in zapis v sensorData (globals)
void updatePeakWatt(); // Decay + preveri nov rekord watt, shrani v NVS če nov rekord
void retrySensors();   // Periodični retry za senzorje ki ob zagonu niso uspeli
bool sensorSht30Ok();  // Stanje inicializacije SHT30
bool sensorIna219Ok(); // Stanje inicializacije INA219
