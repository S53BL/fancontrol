// sensors.h — SHT30 in INA219 branje
#pragma once
#include "config.h"

bool initSensors();    // Inicializacija I2C, SHT30, INA219
void readSensors();    // Branje in zapis v sensorData (globals)
void updatePeakWatt(); // Preveri in posodobi peak watt, shrani v NVS če nov rekord
void retrySensors();   // Periodični retry za senzorje ki ob zagonu niso uspeli
bool sensorSht30Ok();  // Stanje inicializacije SHT30
bool sensorIna219Ok(); // Stanje inicializacije INA219
