// monitor.cpp — Mini PC napajalni + TCP port monitor
#include "monitor.h"
#include "globals.h"
#include "logging.h"
#include <WiFi.h>
#include <WiFiClient.h>

static PortEntry     ports[MONITOR_MAX_PORTS];
static MonitorResult monitorResult;

void monitorInit() {
    portENTER_CRITICAL(&dataMux);
    for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
        ports[i].port    = settings.monitorPorts[i].port;
        memcpy(ports[i].name, settings.monitorPorts[i].name, sizeof(ports[i].name));
        ports[i].enabled = settings.monitorPorts[i].enabled;
        ports[i].lastOk  = false;
    }
    memset(&monitorResult, 0, sizeof(monitorResult));
    portEXIT_CRITICAL(&dataMux);
}

void monitorRun() {
    // Snapshot config + sensor data pod mutex
    PortEntry localPorts[MONITOR_MAX_PORTS];
    char      ipStr[16];
    float     wattThr, watt;
    uint8_t   sensorErr;

    portENTER_CRITICAL(&dataMux);
    memcpy(localPorts, ports, sizeof(localPorts));
    memcpy(ipStr, settings.monitorIp, sizeof(ipStr));
    wattThr   = settings.monitorWattThreshold;
    watt      = sensorData.watt;
    sensorErr = sensorData.err;
    portEXIT_CRITICAL(&dataMux);

    MonitorResult res = {};
    res.powered = !(sensorErr & ERR_INA219) && (watt >= wattThr);

    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN("MON", "WiFi ni povezan — preskoči TCP teste");
        portENTER_CRITICAL(&dataMux);
        monitorResult = res;
        portEXIT_CRITICAL(&dataMux);
        return;
    }

    IPAddress targetIp;
    if (!targetIp.fromString(ipStr)) {
        LOG_WARN("MON", "Neveljaven monitor IP: %s", ipStr);
        portENTER_CRITICAL(&dataMux);
        monitorResult = res;
        portEXIT_CRITICAL(&dataMux);
        return;
    }

    for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
        if (!localPorts[i].enabled || localPorts[i].port == 0) continue;
        res.portCount++;

        WiFiClient client;
        bool ok = (client.connect(targetIp, localPorts[i].port, MONITOR_TCP_TIMEOUT_MS) == 1);
        if (ok) client.stop();

        localPorts[i].lastOk = ok;
        if (ok) res.portOkCount++;
        else    res.anyPortFail = true;
    }

    res.allPortsOk = (res.portCount > 0) && (res.portOkCount == res.portCount);

    // Zapiši rezultate pod mutex
    portENTER_CRITICAL(&dataMux);
    for (int i = 0; i < MONITOR_MAX_PORTS; i++) ports[i].lastOk = localPorts[i].lastOk;
    monitorResult = res;
    portEXIT_CRITICAL(&dataMux);

    LOG_INFO("MON", "powered=%d ports=%d/%d", (int)res.powered, res.portOkCount, res.portCount);
}

MonitorResult monitorGetResult() {
    MonitorResult r;
    portENTER_CRITICAL(&dataMux);
    r = monitorResult;
    portEXIT_CRITICAL(&dataMux);
    return r;
}

PortEntry* monitorGetPorts() {
    return ports;
}
