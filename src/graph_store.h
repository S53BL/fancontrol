// graph_store.h — Krožni buffer za zgodovino meritev (PSRAM)
#pragma once
#include <stdint.h>

struct GraphPoint {
    uint32_t ts;      // Unix timestamp
    float    temp;    // Temperatura [°C]
    float    hum;     // Vlažnost [%]
    float    volt;    // Napetost Mini PC [V]
    float    watt;    // Moč Mini PC [W]
    uint8_t  fanPct;  // Hitrost ventilatorja [%]
    uint8_t  pad[3];  // Poravnava na 24 bytes
};

void       graphStoreInit();
void       graphAddPoint(const GraphPoint& pt);
GraphPoint graphGetPoint(int index);  // 0 = najstarejši
int        graphGetCount();
void       graphStoreClear();
