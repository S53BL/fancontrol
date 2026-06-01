// monitor.h — Mini PC napajalni + TCP port monitor
#pragma once
#include <Arduino.h>
#include "config.h"

struct PortEntry {
    uint16_t port;
    char     name[12];   // "HTTP", "HTTPS", "SMTP" ...
    bool     enabled;
    bool     lastOk;     // rezultat zadnjega TCP preverjanja
};

struct MonitorResult {
    bool    powered;      // W >= settings.monitorWattThreshold
    bool    allPortsOk;   // vsi enabled porti OK
    bool    anyPortFail;  // vsaj en enabled port fail
    uint8_t portCount;    // število enabled portov
    uint8_t portOkCount;  // število OK portov
};

void          monitorInit();
void          monitorRun();           // klic vsako minuto iz loop()
MonitorResult monitorGetResult();
PortEntry*    monitorGetPorts();      // pointer na interno tabelo; beri pod dataMux
