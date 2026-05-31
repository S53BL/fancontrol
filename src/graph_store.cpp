// graph_store.cpp — Implementacija krožnega bufferja v PSRAM
#include "graph_store.h"
#include "config.h"
#include "globals.h"
#include <Arduino.h>

static GraphPoint* _buf   = nullptr;
static int         _head  = 0;
static int         _count = 0;

void graphStoreInit() {
    _buf = (GraphPoint*)ps_malloc(sizeof(GraphPoint) * GRAPH_BUFFER_SIZE);
    if (!_buf) {
        // PSRAM ni dostopen — fallback na heap
        _buf = (GraphPoint*)malloc(sizeof(GraphPoint) * GRAPH_BUFFER_SIZE);
    }
    if (_buf) {
        memset(_buf, 0, sizeof(GraphPoint) * GRAPH_BUFFER_SIZE);
        Serial.printf("[GraphStore] Buffer: %d točk x %d bytes = %d bytes\n",
                      GRAPH_BUFFER_SIZE, sizeof(GraphPoint),
                      GRAPH_BUFFER_SIZE * sizeof(GraphPoint));
    } else {
        Serial.println("[GraphStore] NAPAKA: alokacija bufferja ni uspela!");
    }
    _head = 0; _count = 0;
}

void graphAddPoint(const GraphPoint& pt) {
    if (!_buf) return;
    portENTER_CRITICAL(&dataMux);
    _buf[_head] = pt;
    _head = (_head + 1) % GRAPH_BUFFER_SIZE;
    if (_count < GRAPH_BUFFER_SIZE) _count++;
    portEXIT_CRITICAL(&dataMux);
}

GraphPoint graphGetPoint(int index) {
    // index 0 = najstarejši zapis
    if (!_buf || index < 0 || index >= _count) return {};
    int realIdx = (_head - _count + index + GRAPH_BUFFER_SIZE) % GRAPH_BUFFER_SIZE;
    return _buf[realIdx];
}

int graphGetCount() {
    portENTER_CRITICAL(&dataMux);
    int cnt = _count;
    portEXIT_CRITICAL(&dataMux);
    return cnt;
}

void graphStoreClear() { _head = 0; _count = 0; }
