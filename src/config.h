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
#define FAN_PWM_FREQ        25000   // 25 kHz — nad slišnim pragom (Intel PWM standard)
#define FAN_PWM_RESOLUTION  8       // 8-bit: 0–255
#define FAN_PWM_MIN         0       // 0% (ventilator izklopljen)
#define FAN_PWM_MAX         255     // 100%

// --- Temperaturna krivulja (6 točk, privzeto) ---
#define FAN_CURVE_POINTS    6
#define FAN_CURVE_TEMP_0    28.0f
#define FAN_CURVE_TEMP_1    35.0f
#define FAN_CURVE_TEMP_2    40.0f
#define FAN_CURVE_TEMP_3    46.0f
#define FAN_CURVE_TEMP_4    55.0f
#define FAN_CURVE_TEMP_5    65.0f
#define FAN_CURVE_PCT_0     0
#define FAN_CURVE_PCT_1     2
#define FAN_CURVE_PCT_2     7
#define FAN_CURVE_PCT_3     20
#define FAN_CURVE_PCT_4     50
#define FAN_CURVE_PCT_5     100

// --- Adaptivni fan control ---
#define ADAPT_EMA_ALPHA             0.08f
#define ADAPT_CONFIDENCE_THRESH     20
#define ADAPT_CONFIDENCE_MAX        100
#define ADAPT_EQUILIBRIUM_WINDOW    90
#define ADAPT_TEMP_STABILITY        1.0f
#define ADAPT_MIN_EQUILIBRIUM_MS    600000UL
#define ADAPT_NVS_NAMESPACE         "fanadapt"

// --- Watt Feed-Forward Boost ---
#define BOOST_WATT_THRESHOLD_DEFAULT  17.0f
#define BOOST_PCT_DEFAULT             10
#define BOOST_PCT_MIN                 5
#define BOOST_PCT_MAX                 40
#define BOOST_ACTIVATE_MS             15000UL
#define BOOST_EVAL_MS_DEFAULT         20000UL
#define BOOST_LEARN_MS_DEFAULT        120000UL  // Eval okno za boost samoučenje [ms]
#define BOOST_TEMP_DEADBAND           0.3f
#define BOOST_EMA_ALPHA               0.1f
#define BOOST_NVS_NAMESPACE           "fanboost"

// --- DND (nočni tihi način) ---
#define FAN_DND_MAX_PCT     20      // Max % med DND (USER%; 0 = ugasnjen, >0 = minimalno hlajenje)
#define FAN_DND_HOUR_FROM   22      // Od ure (0–23)
#define FAN_DND_HOUR_TO     6       // Do ure (0–23)

// --- Hysteresis ventilatorja ---
#define FAN_START_PCT       33      // %pwm — kick hitrost za fizični zagon motorja
#define FAN_STOP_PCT        27      // %pwm — minimalna hitrost med tekom motorja
#define FAN_KICK_MS         2000UL  // Trajanje startup kick [ms]

// --- Časovni intervali (ms) ---
#define SENSOR_READ_INTERVAL    10000UL    // Branje senzorjev: 10 s
#define GRAPH_STORE_INTERVAL    10000UL    // Shranjevanje v graf buffer: 10 s
// --- ePaper refresh strategija ---
// Partial refresh (brez flasha) se sproži ob vsaki spremembi minute.
// Periodični full refresh počisti ghosting ki se kopiči s partial osvežitvami.
// Priporočeno: 30 (pogosto vidno ghosting) do 60 (normalno) minut.
#define EPD_FULL_REFRESH_INTERVAL_MIN  60UL   // minute med full refreshi
#define WIFI_CHECK_INTERVAL     600000UL  // WiFi watchdog: 10 min
#define NTP_UPDATE_INTERVAL     1800000UL // NTP sinhronizacija: 30 min
#define SENSOR_REINIT_INTERVAL  60000UL   // Retry inicializacije senzorjev: 60 s
#define NTP_RETRY_INTERVAL      300000UL  // NTP retry če timeSynced=false: 5 min
#define EPD_REINIT_INTERVAL     300000UL  // ePaper reinit retry če ERR_DISPLAY: 5 min

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
#define GRAPH_BUFFER_SIZE   60480   // 7 dni @ 1 točka/10s (60480 točk)

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
#define LOG_BUFFER_ENTRIES  10000      // ~24h log @ ~7 vnosov/min (PSRAM)
#define LOG_MAX_MSG_LEN     120        // Max dolžina enega log sporočila

// --- Mreža ---
#define MDNS_HOSTNAME       "fan"   // http://fan.local

// --- Firmware verzija ---
#define FW_VERSION          "1.3.0"

// --- RGB LED (vgrajena na TZT ESP32-S3-N16R8) ---
// POZOR: GPIO38=FSPIQ in GPIO48=FSPICLK sta rezervirana za OPI PSRAM (qio_opi).
// Preveri schematic plošče — pogosta alternativa je GPIO21.
#define PIN_RGB_LED         48      // Potrebno premostiti mosticek na PCB-ju, da se uporabi GPIO48 
#define LED_BRIGHTNESS      50      // 0–255, nizko da ne slepi

// --- Boot ---
#define BOOT_SERIAL_DELAY_MS   2000   // Čakanje po Serial.begin() za monitor
#define BOOT_SENSOR_DELAY_MS   500    // Čakanje pred I2C init (stabilizacija napajanja)
#define EPD_BUSY_TIMEOUT_MS    3000   // Max čakanje na BUSY LOW pri init [ms]

// --- Open-Meteo vremenski API ---
#define WEATHER_FETCH_INTERVAL  1800000UL  // 30 minut v ms
#define WEATHER_LAT             "46.05"    // Ljubljana — nastavi po potrebi
#define WEATHER_LON             "14.51"    // Ljubljana — nastavi po potrebi
#define WEATHER_URL_TEMPLATE    "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,weather_code&daily=sunrise,sunset&timezone=Europe%%2FLjubljana"
#define WEATHER_BUFFER_SIZE     4096    // PSRAM buffer za weather JSON odgovor

// --- Peak trackerji ---
#define PEAK_WATT_NVS_KEY        "peakWatt"    // NVS: zadnji max watt [W]
#define PEAK_WATT_TS_NVS_KEY     "peakWattTs"  // NVS: unix timestamp zadnjega watt peaka
#define PEAK_WATT_DEFAULT        15.0f         // Privzeti max [W] pri prvem zagonu
#define PEAK_WATT_MIN_FLOOR      5.0f          // Nikoli pod 5W (varnostni floor)
#define PEAK_DECAY_HALFLIFE_DAYS 14.0f         // Watt peak razpolovi se vsakih 14 dni
#define PEAK_TEMP_NVS_KEY        "peakTemp"    // NVS: max temperatura [°C]
#define PEAK_FAN_NVS_KEY         "peakFan"     // NVS: max % ventilatorja

// --- Vremenska ikona skala ---
#define WX_ICON_SCALE           5           // LilyGo Small=10 za 960px; mi=5 za 128px
#define WX_ICON_LINESIZE        2           // Debelina črt ikon

// --- Mini PC Monitor ---
#define MONITOR_TCP_TIMEOUT_MS     2000     // TCP connect timeout [ms] — 2000ms preprečuje false negative
#define MONITOR_MAX_PORTS          9        // Max portov v nastavitvah (3x3 grid)
#define MONITOR_RUN_INTERVAL       300000UL // Skeniranje portov: 5 min

#define MONITOR_DEFAULT_IP         "192.168.2.5"
#define MONITOR_DEFAULT_WATT_THR   3.0f     // Pod tem = Mini PC izklopljen [W]

// Privzeta lista portov (ime max 11 znakov + null)
#define MONITOR_PORT_0    80,   "HTTP",    true
#define MONITOR_PORT_1    443,  "HTTPS",   true
#define MONITOR_PORT_2    25,   "SMTP",    true
#define MONITOR_PORT_3    587,  "SMTP-S",  true
#define MONITOR_PORT_4    465,  "SMTPS",   true
#define MONITOR_PORT_5    993,  "IMAP-S",  true
#define MONITOR_PORT_6    9443, "Axigen", true
#define MONITOR_PORT_7    0,    "",        false  // opcijski slot 1
#define MONITOR_PORT_8    0,    "",        false  // opcijski slot 2

// --- WiFi Intelligence ---
#define WIFI_ROAM_INTERVAL_MS       3600000UL  // Periodični roaming reconnect: 60 minut
#define WIFI_NTP_FAIL_THRESHOLD     3           // Consecutive NTP napake pred WiFi reconnectom
#define WIFI_SCAN_HISTORY_SIZE      10          // Število shranjenih scan/reconnect eventov
#define WIFI_SCAN_MAX_APS           20          // Max AP-jev v scan rezultatih

// --- Hardware Watchdog ---
#define WDT_TIMEOUT_S               120         // Task watchdog timeout [s]

// --- WiFi omrežja ---
#define WIFI_SLOT_COUNT             5           // Število WiFi slotov

// --- NanoPi klient ---
#define NANOPI_FETCH_INTERVAL_DEFAULT  60000UL  // Privzeti interval klica NanoPi [ms]
#define NANOPI_HTTP_TIMEOUT_MS         5000      // HTTP timeout za klic NanoPi [ms]
#define NANOPI_STALE_THRESHOLD_MS      300000UL  // 5 minut — podatki so zastareli [ms]
#define NANOPI_FAIL_THRESHOLD          5          // Zaporednih napak preden prikaže opozorilo
#define NANOPI_DEFAULT_IP              "192.168.2.9"
