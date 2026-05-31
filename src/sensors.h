// sensors.h — SHT30 in INA219 branje
#pragma once
#include "config.h"

bool initSensors();   // Inicializacija I2C, SHT30, INA219
void readSensors();   // Branje in zapis v sensorData (globals)
