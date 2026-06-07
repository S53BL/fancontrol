// nanopi_client.cpp — HTTP klient za NanoPi R3S /api/fandata
#include "nanopi_client.h"
#include "globals.h"
#include "logging.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static NanoPiData _nanopiData = {};
static unsigned long _lastFetchMs = 0;

void nanopiClientInit() {
    portENTER_CRITICAL(&dataMux);
    memset(&_nanopiData, 0, sizeof(_nanopiData));
    _nanopiData.valid     = false;
    _nanopiData.stale     = true;
    _nanopiData.failCount = 0;
    portEXIT_CRITICAL(&dataMux);
    _lastFetchMs = 0;
    LOG_INFO("NANOPI", "Klient init OK — interval=%lums  IP=%s",
             (unsigned long)settings.nanopiIntervalMs, settings.nanopiIp);
}

NanoPiData nanopiGetData() {
    NanoPiData d;
    portENTER_CRITICAL(&dataMux);
    d = _nanopiData;
    portEXIT_CRITICAL(&dataMux);
    return d;
}

static void _doFetch() {
    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN("NANOPI", "WiFi ni povezan — preskok fetcha");
        portENTER_CRITICAL(&dataMux);
        _nanopiData.failCount++;
        portEXIT_CRITICAL(&dataMux);
        return;
    }

    char url[48];
    portENTER_CRITICAL(&dataMux);
    snprintf(url, sizeof(url), "http://%s/api/fandata", settings.nanopiIp);
    portEXIT_CRITICAL(&dataMux);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(NANOPI_HTTP_TIMEOUT_MS);
    int code = http.GET();

    if (code != 200) {
        LOG_WARN("NANOPI", "HTTP %d za %s", code, url);
        http.end();
        portENTER_CRITICAL(&dataMux);
        _nanopiData.failCount++;
        // Preveri stale
        if (millis() - _nanopiData.ts * 1000UL > NANOPI_STALE_THRESHOLD_MS || !_nanopiData.valid) {
            _nanopiData.stale = true;
        }
        portEXIT_CRITICAL(&dataMux);
        return;
    }

    String payload = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        LOG_WARN("NANOPI", "JSON parse napaka: %s", err.c_str());
        portENTER_CRITICAL(&dataMux);
        _nanopiData.failCount++;
        if (millis() - _nanopiData.ts * 1000UL > NANOPI_STALE_THRESHOLD_MS || !_nanopiData.valid) {
            _nanopiData.stale = true;
        }
        portEXIT_CRITICAL(&dataMux);
        return;
    }

    // Parse uspešen — posodobi podatke
    NanoPiData fresh = {};
    fresh.ts                  = doc["ts"] | (uint32_t)0;
    fresh.networkCurrentMbits = doc["network"]["current_mbits"] | 0.0f;
    fresh.networkPeakMbits    = doc["network"]["peak_mbits"]    | 0.0f;
    fresh.networkBarPct       = doc["network"]["bar_pct"]       | (uint8_t)0;
    fresh.attacksCurrentPh    = doc["attacks"]["current_ph"]    | (uint32_t)0;
    fresh.attacksPeakPh       = doc["attacks"]["peak_ph"]       | (uint32_t)0;
    fresh.attacksBarPct       = doc["attacks"]["bar_pct"]       | (uint8_t)0;
    fresh.banipRunning        = doc["system"]["banip_running"]  | false;
    fresh.banipLastReload     = doc["system"]["banip_last_reload"] | (uint32_t)0;
    fresh.serverOk            = doc["system"]["server_ok"]      | false;
    fresh.valid               = true;
    fresh.stale               = false;
    fresh.failCount           = 0;

    portENTER_CRITICAL(&dataMux);
    _nanopiData = fresh;
    portEXIT_CRITICAL(&dataMux);

    LOG_INFO("NANOPI", "Fetch OK — promet=%.1f Mbit/s  napadi=%lu/h  server_ok=%d",
             fresh.networkCurrentMbits, (unsigned long)fresh.attacksCurrentPh, (int)fresh.serverOk);
}

void nanopiClientLoop() {
    unsigned long now = millis();
    uint32_t interval;
    portENTER_CRITICAL(&dataMux);
    interval = settings.nanopiIntervalMs;
    portEXIT_CRITICAL(&dataMux);

    if (now - _lastFetchMs >= interval) {
        _lastFetchMs = now;
        _doFetch();
    }
}
