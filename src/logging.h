// logging.h — Enostaven RAM log buffer za fancontrol
// Shranjuje zadnjih LOG_BUFFER_ENTRIES vnosov v PSRAM krožni buffer
// Po resetu prazen. Brez SD, brez datotek.

#pragma once
#include "config.h"

// Nivoji
enum LogLevel : uint8_t {
    LOG_LVL_INFO  = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_ERROR = 2,
};

// En vnos
struct LogEntry {
    char     time[10];   // "HH:MM:SS\0"
    LogLevel level;
    char     tag[12];    // npr. "FAN\0", "SENS\0"
    char     msg[LOG_MAX_MSG_LEN];
};

// Inicializacija (klic iz setup() pred vsem drugim)
void logInit();

// Dodaj vnos (thread-safe)
void logAdd(LogLevel level, const char* tag, const char* fmt, ...);

// Dostop do bufferja za web prikaz
int        logGetCount();
LogEntry   logGetEntry(int index);   // 0 = najstarejši
void       logClear();

// Makri za udobno uporabo
#define LOG_INFO(tag, fmt, ...)  logAdd(LOG_LVL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  logAdd(LOG_LVL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) logAdd(LOG_LVL_ERROR, tag, fmt, ##__VA_ARGS__)
