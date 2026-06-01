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
// WeAct pin connector vrstni red: 1=BUSY 2=RES 3=D/C 4=CS 5=SCL 6=SDA 7=GND 8=VCC
// POZOR: WeAct poimenuje SPI pine z I2C imeni: SDA=MOSI, SCL=CLK
#define PIN_EPD_BUSY        15   // WeAct pin 1 — Busy signal (čakanje na refresh)
#define PIN_EPD_RST         14   // WeAct pin 2 — Reset (RES, aktiven LOW)
#define PIN_EPD_DC          13   // WeAct pin 3 — Data/Command
#define PIN_EPD_CS          10   // WeAct pin 4 — Chip select (aktiven LOW)
#define PIN_EPD_CLK         12   // WeAct pin 5 — SPI clock (SCL=CLK)
#define PIN_EPD_MOSI        11   // WeAct pin 6 — SPI data (SDA=MOSI)

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

// --- Logging ---
#define LOG_BUFFER_ENTRIES  200        // Število vnosov v RAM log bufferju (PSRAM)
#define LOG_MAX_MSG_LEN     120        // Max dolžina enega log sporočila

// --- Mreža ---
#define STATIC_IP           "192.168.2.169"
#define STATIC_GW           "192.168.2.1"
#define STATIC_SUBNET       "255.255.255.0"
#define STATIC_DNS          "8.8.8.8"
#define MDNS_HOSTNAME       "fancontrol"   // http://fancontrol.local

// --- Firmware verzija ---
#define FW_VERSION          "0.7.0"

// --- RGB LED (vgrajena na TZT ESP32-S3-N16R8) ---
// POZOR: GPIO38=FSPIQ in GPIO48=FSPICLK sta rezervirana za OPI PSRAM (qio_opi).
// Preveri schematic plošče — pogosta alternativa je GPIO21.
#define PIN_RGB_LED         21      // TODO: preveri na schematiku TZT plošče
#define RGB_BRIGHTNESS      50      // 0–255, nizko da ne slepi

// --- Boot ---
#define BOOT_SERIAL_DELAY_MS   2000   // Čakanje po Serial.begin() za monitor
#define BOOT_SENSOR_DELAY_MS   500    // Čakanje pred I2C init (stabilizacija napajanja)
#define EPD_BUSY_TIMEOUT_MS    3000   // Max čakanje na BUSY LOW pri init [ms]

// --- Open-Meteo vremenski API ---
#define WEATHER_FETCH_INTERVAL  1800000UL  // 30 minut v ms
#define WEATHER_LAT             "46.05"    // Ljubljana — nastavi po potrebi
#define WEATHER_LON             "14.51"    // Ljubljana — nastavi po potrebi
#define WEATHER_URL_TEMPLATE    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,weather_code"

// --- Peak watt avtokalibracija ---
#define PEAK_WATT_NVS_KEY       "peakWatt"  // Ključ v NVS za persistenco
#define PEAK_WATT_DEFAULT       15.0f       // Privzeti max [W] pri prvem zagonu
#define PEAK_WATT_MIN_FLOOR     5.0f        // Nikoli pod 5W (varnostni floor)

// --- Vremenska ikona skala ---
#define WX_ICON_SCALE           5           // LilyGo Small=10 za 960px; mi=5 za 128px
#define WX_ICON_LINESIZE        2           // Debelina črt ikon

// --- Mini PC Monitor ---
#define MONITOR_TCP_TIMEOUT_MS     500      // TCP connect timeout [ms]
#define MONITOR_MAX_PORTS          8        // Max portov v nastavitvah

#define MONITOR_DEFAULT_IP         "192.168.2.5"
#define MONITOR_DEFAULT_WATT_THR   3.0f     // Pod tem = Mini PC izklopljen [W]

// Privzeta lista portov (ime max 11 znakov + null)
#define MONITOR_PORT_0    80,   "HTTP",    true
#define MONITOR_PORT_1    443,  "HTTPS",   true
#define MONITOR_PORT_2    25,   "SMTP",    true
#define MONITOR_PORT_3    587,  "SMTP-S",  true
#define MONITOR_PORT_4    465,  "SMTPS",   true
#define MONITOR_PORT_5    993,  "IMAP-S",  true
#define MONITOR_PORT_6    8443, "Webmail", true
