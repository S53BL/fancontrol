// config.h — Centralna konfiguracija za fancontrol
// Plošča: TZT ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM OPI)
// POZOR: Samo pinout, konstante in enum-i. Podatkovne strukture so v globals.h.

#pragma once
#include <Arduino.h>

// =============================================================================
// PINOUT — TZT ESP32-S3-N16R8
// Vsi pini so definirani tukaj. Nikjer v kodi ne smeš pisati številk direktno.
// =============================================================================

// --- I2C bus (SHT30 + INA219) ---
#define PIN_I2C_SDA         8
#define PIN_I2C_SCL         9

// --- SPI bus — ePaper WeAct 2.9" B/W ---
#define PIN_EPD_MOSI        11
#define PIN_EPD_CLK         12
#define PIN_EPD_CS          10
#define PIN_EPD_DC          13
#define PIN_EPD_RST         14
#define PIN_EPD_BUSY        15

// --- Ventilator PWM → LR7843 MOSFET modul ---
#define PIN_FAN_PWM         5

// --- I2C naslovi ---
#define ADDR_SHT30          0x44
#define ADDR_INA219         0x40

// --- PWM konfiguracija ventilatorja ---
#define FAN_PWM_CHANNEL     0
#define FAN_PWM_FREQ        1000    // 1 kHz — primerno za 2-pin ventilator
#define FAN_PWM_RESOLUTION  8       // 8-bit: 0–255
#define FAN_PWM_MIN         0       // 0% (ventilator izklopljen)
#define FAN_PWM_MAX         255     // 100%

// --- Temperaturna krivulja (privzeto, nastavljivo v web vmesniku) ---
#define FAN_CURVE_POINTS    4
// Temp [°C] : pod to temp → ta % PWM
#define FAN_CURVE_TEMP_0    35.0f
#define FAN_CURVE_TEMP_1    45.0f
#define FAN_CURVE_TEMP_2    55.0f
#define FAN_CURVE_TEMP_3    60.0f
#define FAN_CURVE_PCT_0     20      // %
#define FAN_CURVE_PCT_1     50
#define FAN_CURVE_PCT_2     80
#define FAN_CURVE_PCT_3     100

// --- DND (nočni tihi način) ---
#define FAN_DND_MAX_PCT     30      // Max % med DND
#define FAN_DND_HOUR_FROM   22      // Od ure (0–23)
#define FAN_DND_HOUR_TO     7       // Do ure (0–23)

// --- Minimalna hitrost ventilatorja ---
#define FAN_MIN_PCT         10      // % — preprečevanje popolnega izklopa

// --- Časovni intervali (ms) ---
#define SENSOR_READ_INTERVAL    10000UL    // Branje senzorjev: 10 s
#define GRAPH_STORE_INTERVAL    60000UL    // Shranjevanje v graf buffer: 60 s
#define DISPLAY_REFRESH_INTERVAL 60000UL  // ePaper osvežitev: 60 s
#define WIFI_CHECK_INTERVAL     600000UL  // WiFi watchdog: 10 min
#define NTP_UPDATE_INTERVAL     1800000UL // NTP sinhronizacija: 30 min

// --- Časovna cona ---
#define TZ_STRING   "CET-1CEST,M3.5.0,M10.5.0/3"

// --- UART ---
#define SERIAL_BAUD         115200

// --- NVS namespace ---
#define NVS_NAMESPACE       "fanctrl"

// --- ePaper dimenzije (pokončna orientacija) ---
#define EPD_WIDTH           128
#define EPD_HEIGHT          296

// --- Graf buffer (PSRAM) ---
#define GRAPH_BUFFER_SIZE   60      // 60 točk = 60 minut

// --- Napake (bitmask) ---
enum ErrorFlag : uint8_t {
    ERR_NONE    = 0x00,
    ERR_SHT30   = 0x01,
    ERR_INA219  = 0x02,
    ERR_WIFI    = 0x04,
    ERR_NTP     = 0x08,
    ERR_DISPLAY = 0x10,
};

// --- Sentinel vrednosti ---
#define ERR_FLOAT   -999.0f
