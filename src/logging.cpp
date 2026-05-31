// logging.cpp — Implementacija RAM log bufferja
// PSRAM alokacija z ps_malloc(), fallback na heap
// Thread-safe z lastnim mutex (ne dataMux — logging teče iz vseh kontekstov)

#include "logging.h"
#include "globals.h"
#include <Arduino.h>
#include <stdarg.h>
#include <ezTime.h>

static LogEntry*    _buf   = nullptr;
static int          _head  = 0;
static int          _count = 0;
static portMUX_TYPE _logMux = portMUX_INITIALIZER_UNLOCKED;

void logInit() {
    size_t sz = sizeof(LogEntry) * LOG_BUFFER_ENTRIES;
    _buf = (LogEntry*)ps_malloc(sz);
    if (!_buf) _buf = (LogEntry*)malloc(sz);
    if (_buf) memset(_buf, 0, sz);
    _head = 0; _count = 0;
}

void logAdd(LogLevel level, const char* tag, const char* fmt, ...) {
    if (!_buf) return;

    LogEntry e;
    memset(&e, 0, sizeof(e));

    // Čas: hh:mm:ss iz ezTime če je synced, sicer millis
    if (timeSynced) {
        snprintf(e.time, sizeof(e.time), "%02d:%02d:%02d",
                 myTZ.hour(), myTZ.minute(), myTZ.second());
    } else {
        unsigned long s = millis() / 1000;
        snprintf(e.time, sizeof(e.time), "%02lu:%02lu:%02lu",
                 (s / 3600) % 24, (s / 60) % 60, s % 60);
    }

    e.level = level;
    strncpy(e.tag, tag ? tag : "?", sizeof(e.tag) - 1);

    va_list args;
    va_start(args, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, args);
    va_end(args);

    // Thread-safe vpis
    portENTER_CRITICAL(&_logMux);
    _buf[_head] = e;
    _head = (_head + 1) % LOG_BUFFER_ENTRIES;
    if (_count < LOG_BUFFER_ENTRIES) _count++;
    portEXIT_CRITICAL(&_logMux);

    // Tudi na Serial za debug
    const char* lvlStr = (level == LOG_LVL_ERROR) ? "ERR" :
                         (level == LOG_LVL_WARN)  ? "WRN" : "INF";
    Serial.printf("[%s][%s][%s] %s\n", e.time, lvlStr, e.tag, e.msg);
}

int logGetCount() {
    int c;
    portENTER_CRITICAL(&_logMux);
    c = _count;
    portEXIT_CRITICAL(&_logMux);
    return c;
}

LogEntry logGetEntry(int index) {
    LogEntry e = {};
    portENTER_CRITICAL(&_logMux);
    if (_buf && index >= 0 && index < _count) {
        int real = (_head - _count + index + LOG_BUFFER_ENTRIES) % LOG_BUFFER_ENTRIES;
        e = _buf[real];
    }
    portEXIT_CRITICAL(&_logMux);
    return e;
}

void logClear() {
    portENTER_CRITICAL(&_logMux);
    _head = 0; _count = 0;
    portEXIT_CRITICAL(&_logMux);
}
