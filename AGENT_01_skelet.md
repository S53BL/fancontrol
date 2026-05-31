# AGENT NAVODILO — Faza 1: Skelet projekta
**Projekt:** fancontrol (pametni ventilator za ACEMAGIC S1)
**Referenca (samo branje):** `C:\PlatformIO\Projekti\vent_SEW`
**Cilj:** `C:\PlatformIO\Projekti\fancontrol`

---

## Splošna pravila — preberi preden začneš

- Referenčni projekt vent_SEW **bereš, nikoli pišeš vanj**
- Iz vent_SEW prevzemaš **vzorce in strukture**, ne kopiraj kode dobesedno
- Naš projekt je **bistveno enostavnejši** — nič kamere, nič LVGL, nič touch, nič SD, nič BME680, nič TCS, nič PIR
- Vse spremenljivke pinov in konstant daj v `config.h` — **nikjer hardcoded številk**
- Komentarji v slovenščini, koda v angleščini (spremenljivke, funkcije)
- Po končani fazi **prevedi projekt** (`pio run`) in popravi vse napake preden poročaš

---

## Ciljna struktura datotek

Ustvari točno te datoteke v `C:\PlatformIO\Projekti\fancontrol`:

```
fancontrol/
├── platformio.ini
├── partitions_16mb_ota.csv
├── CHANGELOG.md
├── to-do.md
├── decisions.md
└── src/
    ├── config.h
    ├── globals.h
    ├── globals.cpp
    ├── sensors.h
    ├── sensors.cpp
    ├── fan.h
    ├── fan.cpp
    ├── display.h
    ├── display.cpp
    ├── webserver.h
    ├── webserver.cpp
    ├── graph_store.h
    ├── graph_store.cpp
    └── main.cpp
```

---

## 1. platformio.ini

Vzorec: `C:\PlatformIO\Projekti\vent_SEW\platformio.ini`

Ohrani:
- `platform`, `platform_packages`, `board = esp32-s3-devkitc-1`, `framework`
- `monitor_speed`, `monitor_filters`, `monitor_dtr/rts`, `upload_speed`
- `board_build.flash_mode = qio`
- `board_build.arduino.memory_type = qio_opi`
- `board_build.psram_type = opi`
- `-DBOARD_HAS_PSRAM`, `-DARDUINO_USB_CDC_ON_BOOT=1`, `-DARDUINO_USB_MODE=1`
- `-DCORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_INFO`, `-std=c++17`, `-I.`, `-Isrc`
- `me-no-dev/AsyncTCP`, `me-no-dev/ESPAsyncWebServer`
- `bblanchon/ArduinoJson @ ^6.21.0`
- `ropg/ezTime`
- `adafruit/Adafruit BusIO @ ^1.14.5`

Zamenjaj/dodaj:
- `board_build.partitions = partitions_16mb_ota.csv`
- `board_build.flash_size = 16MB` (ne 8MB)
- `board_build.filesystem` — **odstrani** (ni LittleFS)
- `-DARDUINO_LOOP_STACK_SIZE=8192` (dovolj za naš projekt)

Dodaj nove knjižnjice:
```ini
zinggjm/GxEPD2 @ ^1.6.0
adafruit/Adafruit SHT31 Library @ ^2.2.2
adafruit/Adafruit INA219 @ ^1.2.3
```

Odstrani popolnoma:
- `espressif/esp32-camera`
- `sensirion/Sensirion I2C SHT4x`
- `Bosch-BSEC2-Library`, `Bosch-BME68x-Library`
- `adafruit/Adafruit TCS34725`
- `lvgl/lvgl`
- `-DLV_CONF_INCLUDE_SIMPLE`

Dodaj `[env:esp32s3_debug]` blok identičen vzorcu iz vent_SEW (brez sprememb).

---

## 2. partitions_16mb_ota.csv

Ustvari novo datoteko (ne kopiraj iz vent_SEW — tam je 8MB verzija).

Vsebina:
```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
ota_0,    app,  ota_0,   0x10000,  0x680000,
ota_1,    app,  ota_1,   0x690000, 0x680000,
coredump, data, coredump,0xD10000, 0x10000,
```

---

## 3. src/config.h

**Nova datoteka** — ne kopiraj iz vent_SEW (tam je Waveshare Touch LCD pinout, ki ga ne rabimo).

Vsebina — natančno to:

```cpp
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
```

---

## 4. src/globals.h

Vzorec: `C:\PlatformIO\Projekti\vent_SEW\src\globals.h`

Ohrani vzorec (struktura, NVS load/save, mutex), a **povsem zamenjaj vsebino** glede na naš projekt:

```cpp
// globals.h — Globalne spremenljivke za fancontrol
#pragma once
#include "config.h"
#include <Preferences.h>
#include <ezTime.h>

// --- Senzorski podatki ---
struct SensorData {
    float    temp;      // Temperatura [°C] — SHT30
    float    hum;       // Relativna vlažnost [%] — SHT30
    float    volt;      // Napetost Mini PC [V] — INA219
    float    amp;       // Tok Mini PC [A] — INA219
    float    watt;      // Moč Mini PC [W] — INA219
    uint8_t  fanPct;    // Hitrost ventilatorja [%]
    bool     dndActive; // DND način aktiven
    uint8_t  err;       // Bitmask napak (ErrorFlag)
};

// --- Nastavitve (NVS) ---
struct Settings {
    // WiFi
    char     ssid[32];
    char     password[64];
    // Temperaturna krivulja (4 točke)
    float    curveTemp[FAN_CURVE_POINTS];
    uint8_t  curvePct[FAN_CURVE_POINTS];
    // DND
    bool     dndEnabled;
    uint8_t  dndFrom;   // ura 0–23
    uint8_t  dndTo;     // ura 0–23
    uint8_t  dndMaxPct; // max % med DND
    // Minimalna hitrost
    uint8_t  fanMinPct;
};

// --- Extern deklaracije ---
extern SensorData sensorData;
extern Settings   settings;
extern Timezone   myTZ;
extern bool       timeSynced;
extern bool       newSensorData;

// Timing
extern unsigned long lastSensorReadMs;
extern unsigned long lastGraphStoreMs;
extern unsigned long lastDisplayRefreshMs;
extern unsigned long lastWifiCheckMs;
extern unsigned long lastNtpSyncMs;

// Mutex za thread-safety (Core 0 = web, Core 1 = senzorji/fan)
extern portMUX_TYPE dataMux;

// --- Funkcije ---
void initGlobals();
void loadSettings();
void saveSettings();
void resetSettings();
```

---

## 5. src/globals.cpp

Vzorec: `C:\PlatformIO\Projekti\vent_SEW\src\globals.cpp`

Ohrani vzorec: CRC16 zaščita NVS, load/save/reset pattern, initGlobals() struktura.
Zamenjaj: vse spremenljivke in strukture za naš projekt (SensorData, Settings).
Odstrani: vse kar se nanaša na SEW (unitId, rewIP, SD mutex, logiranje, kamera).

Privzete vrednosti za Settings:
- `ssid` / `password`: prazna stringa `""`
- `curveTemp`: `{35.0, 45.0, 55.0, 60.0}`
- `curvePct`: `{20, 50, 80, 100}`
- `dndEnabled`: `false`, `dndFrom`: `22`, `dndTo`: `7`, `dndMaxPct`: `30`
- `fanMinPct`: `10`

---

## 6. src/sensors.h in sensors.cpp

**Nova datoteka** — ne kopiraj iz vent_SEW sens.h/cpp (tam je SHT41+BME680+TCS+PIR+baterija ADC).

`sensors.h` — samo deklaracije:
```cpp
// sensors.h — SHT30 in INA219 branje
#pragma once
#include "config.h"

bool initSensors();   // Inicializacija I2C, SHT30, INA219
void readSensors();   // Branje in zapis v sensorData (globals)
```

`sensors.cpp` — skeleton implementacija:
```cpp
// sensors.cpp — Implementacija branja SHT30 in INA219
#include "sensors.h"
#include "globals.h"
#include <Adafruit_SHT31.h>
#include <Adafruit_INA219.h>
#include <Wire.h>

static Adafruit_SHT31  sht30;
static Adafruit_INA219 ina219(ADDR_INA219);

bool initSensors() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    // TODO: inicializacija SHT30 in INA219
    return true;
}

void readSensors() {
    // TODO: branje temp/hum iz SHT30, V/A/W iz INA219
    // Zapiši v sensorData z mutex zaščito
}
```

---

## 7. src/fan.h in fan.cpp

**Nova datoteka.**

`fan.h`:
```cpp
// fan.h — PWM krmiljenje ventilatorja
#pragma once
#include "config.h"

void initFan();           // Inicializacija LEDC PWM
void updateFan();         // Izračun in nastavitev hitrosti glede na temp + DND
void setFanPct(uint8_t pct); // Direktna nastavitev [0–100%]
uint8_t getFanPct();      // Trenutna hitrost [%]
bool isDndActive();       // Je DND aktiven glede na čas
```

`fan.cpp` — skeleton:
```cpp
// fan.cpp — Implementacija PWM krmiljenja ventilatorja
#include "fan.h"
#include "globals.h"
#include <Arduino.h>

static uint8_t _fanPct = 0;

void initFan() {
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, FAN_PWM_CHANNEL);
    setFanPct(0);
}

void setFanPct(uint8_t pct) {
    _fanPct = constrain(pct, 0, 100);
    uint8_t duty = map(_fanPct, 0, 100, FAN_PWM_MIN, FAN_PWM_MAX);
    ledcWrite(FAN_PWM_CHANNEL, duty);
    portENTER_CRITICAL(&dataMux);
    sensorData.fanPct = _fanPct;
    portEXIT_CRITICAL(&dataMux);
}

uint8_t getFanPct() { return _fanPct; }

bool isDndActive() {
    // TODO: preveri čas glede na settings.dndFrom/To
    return false;
}

void updateFan() {
    // TODO: preračun po temperaturni krivulji + DND logika
}
```

---

## 8. src/display.h in display.cpp

**Nova datoteka.**

`display.h`:
```cpp
// display.h — GxEPD2 ePaper WeAct 2.9" B/W
#pragma once

void initDisplay();    // Inicializacija GxEPD2
void updateDisplay();  // Full refresh zaslona
void partialDisplay(); // Partial refresh (če podprt)
```

`display.cpp` — skeleton:
```cpp
// display.cpp — Implementacija ePaper zaslona
// WeAct Studio 2.9" B/W, 296x128 px, pokončna orientacija
// Knjižnjica: GxEPD2 — inicializacijo prevzemi iz WeAct primera za GxEPD2_290_T94_V2
#include "display.h"
#include "config.h"
#include "globals.h"
#include <GxEPD2_BW.h>

// TODO: deklariraj display objekt glede na WeAct 2.9" B/W kontroler
// Primer: GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(...)

void initDisplay() {
    // TODO
}

void updateDisplay() {
    // TODO: izriši vse 4 cone (čas, temp/vlaga, V/A/W, fan%)
}

void partialDisplay() {
    // TODO
}
```

---

## 9. src/webserver.h in webserver.cpp

**Nova datoteka** — inspiracija iz VAC_Plug vzorca (buildPage() v stringu) +
vent_SEW web.cpp/http.cpp za AsyncWebServer setup in API endpointe.

`webserver.h`:
```cpp
// webserver.h — AsyncWebServer za fancontrol
#pragma once

void initWebserver();   // WiFi connect + server.begin() + registracija endpointov
void handleWebserver(); // Klic v loop() (ni potreben za Async, a za mDNS)
```

`webserver.cpp` — skeleton:
```cpp
// webserver.cpp — Web vmesnik za fancontrol
// Arhitektura: HTML vgrajen v firmware (buildPage()), brez LittleFS
// Vzorec: VAC_Plug (buildPage, /save POST, Preferences) + vent_SEW (API endpointi)
#include "webserver.h"
#include "globals.h"
#include "config.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static AsyncWebServer server(80);
static portMUX_TYPE _webMux = portMUX_INITIALIZER_UNLOCKED;

static String buildPage() {
    // TODO: celoten HTML z Chart.js grafi, 3 tabi (Dashboard, Nastavitve, Sistem)
    return "<html><body><h1>fancontrol — v0.1</h1><p>TODO</p></body></html>";
}

void initWebserver() {
    // TODO: WiFi.begin(), čakaj na connect, nastavi mDNS
    // Endpointi:
    //   GET  /           → buildPage()
    //   GET  /api/data   → JSON trenutnih vrednosti
    //   GET  /api/history → JSON array za grafe
    //   POST /save       → shrani nastavitve v NVS
    //   GET  /update     → OTA HTML
    //   POST /update     → OTA flash
    server.begin();
}

void handleWebserver() {
    // mDNS update — klic v loop()
}
```

---

## 10. src/graph_store.h in graph_store.cpp

Vzorec: `C:\PlatformIO\Projekti\vent_SEW\src\graph_store.h` in `graph_store.cpp`

Ohrani vzorec: krožni buffer, graphAddPoint(), graphGetPoint(), graphGetCount().
Zamenjaj struct GraphPoint za naš projekt:

```cpp
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

void     graphStoreInit();
void     graphAddPoint(const GraphPoint& pt);
GraphPoint graphGetPoint(int index);  // 0 = najstarejši
int      graphGetCount();
void     graphStoreClear();
```

`graph_store.cpp` — buffer mora biti v PSRAM:
```cpp
// graph_store.cpp — Implementacija krožnega bufferja v PSRAM
// Vzorec: vent_SEW graph_store.cpp (krožni buffer, head/count logika)
// PSRAM alokacija: ps_malloc() — ne navadni malloc()!
#include "graph_store.h"
#include "config.h"
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
    _head = 0; _count = 0;
}

void graphAddPoint(const GraphPoint& pt) {
    if (!_buf) return;
    _buf[_head] = pt;
    _head = (_head + 1) % GRAPH_BUFFER_SIZE;
    if (_count < GRAPH_BUFFER_SIZE) _count++;
}

GraphPoint graphGetPoint(int index) {
    // index 0 = najstarejši zapis
    if (!_buf || index < 0 || index >= _count) return {};
    int realIdx = (_head - _count + index + GRAPH_BUFFER_SIZE) % GRAPH_BUFFER_SIZE;
    return _buf[realIdx];
}

int graphGetCount() { return _count; }
void graphStoreClear() { _head = 0; _count = 0; }
```

---

## 11. src/main.cpp

Vzorec: `C:\PlatformIO\Projekti\vent_SEW\src\main.cpp`

Ohrani vzorec: setup() koraki, loop() timing logika, WiFi, NTP, multi-core komentarji.
Zamenjaj: vse SEW specifično. Odstrani: kamera, SD, logging modul, weather, LVGL.

```cpp
// main.cpp — fancontrol glavna zanka
// Plošča: TZT ESP32-S3-N16R8
// Core 0: WiFi stack + AsyncWebServer
// Core 1: senzorji, PWM fan, ePaper (loop)

#include <Arduino.h>
#include "config.h"
#include "globals.h"
#include "sensors.h"
#include "fan.h"
#include "display.h"
#include "webserver.h"
#include "graph_store.h"
#include <WiFi.h>
#include <ezTime.h>

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println("\n=== fancontrol boot ===");

    initGlobals();       // NVS settings + sensorData init
    graphStoreInit();    // PSRAM buffer
    initFan();           // PWM init — ventilator na 0%
    initSensors();       // I2C + SHT30 + INA219
    initDisplay();       // ePaper init
    initWebserver();     // WiFi + NTP + AsyncWebServer

    Serial.println("=== Boot complete ===");
}

void loop() {
    unsigned long now = millis();

    // Branje senzorjev
    if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL) {
        lastSensorReadMs = now;
        readSensors();
        updateFan();
        newSensorData = true;
    }

    // Shranjevanje v graf buffer
    if (newSensorData && (now - lastGraphStoreMs >= GRAPH_STORE_INTERVAL)) {
        lastGraphStoreMs = now;
        newSensorData = false;
        GraphPoint pt;
        portENTER_CRITICAL(&dataMux);
        pt.ts     = (uint32_t)time(nullptr);
        pt.temp   = sensorData.temp;
        pt.hum    = sensorData.hum;
        pt.volt   = sensorData.volt;
        pt.watt   = sensorData.watt;
        pt.fanPct = sensorData.fanPct;
        portEXIT_CRITICAL(&dataMux);
        graphAddPoint(pt);
    }

    // ePaper osvežitev
    if (now - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL) {
        lastDisplayRefreshMs = now;
        updateDisplay();
    }

    // WiFi watchdog
    if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = now;
        // TODO: reconnect če ni WiFi
    }

    handleWebserver(); // mDNS
    delay(10);
}
```

---

## 12. Dokumentacijski fajli

Ustvari v `C:\PlatformIO\Projekti\fancontrol`:

**CHANGELOG.md:**
```markdown
# Changelog — fancontrol

## [0.1.0] — 2026-05-31
### Dodano
- Skelet projekta: platformio.ini, particijska tabela, vse src/ datoteke
- config.h: pinout TZT ESP32-S3-N16R8, konstante, enumi
- globals.h/cpp: SensorData, Settings, NVS load/save z CRC16
- sensors.h/cpp: skeleton SHT30 + INA219
- fan.h/cpp: skeleton PWM krmiljenje, temperaturna krivulja
- display.h/cpp: skeleton GxEPD2 WeAct 2.9" B/W
- webserver.h/cpp: skeleton AsyncWebServer, buildPage(), API endpointi
- graph_store.h/cpp: krožni PSRAM buffer, GraphPoint struktura
- main.cpp: setup/loop skeleton, timing logika
```

**to-do.md:**
```markdown
# To-Do — fancontrol

## Faza 2 — Implementacija modulov
- [ ] sensors.cpp: SHT30 branje (Adafruit_SHT31)
- [ ] sensors.cpp: INA219 branje (Adafruit_INA219)
- [ ] fan.cpp: temperaturna krivulja po točkah
- [ ] fan.cpp: DND logika z ezTime
- [ ] display.cpp: GxEPD2 init za WeAct 2.9" B/W
- [ ] display.cpp: izris 4 con (čas, temp/vlaga, V/A/W, fan%)
- [ ] webserver.cpp: WiFi connect + NTP
- [ ] webserver.cpp: buildPage() — Dashboard tab z grafi (Chart.js)
- [ ] webserver.cpp: buildPage() — Nastavitve tab
- [ ] webserver.cpp: buildPage() — Sistem tab (OTA)
- [ ] webserver.cpp: /api/data JSON endpoint
- [ ] webserver.cpp: /api/history JSON endpoint
- [ ] webserver.cpp: /save POST + NVS shranjevanje
- [ ] webserver.cpp: OTA (ArduinoOTA ali ElegantOTA)
- [ ] main.cpp: WiFi reconnect logika
- [ ] Testiranje na hardware
```

**decisions.md:**
```markdown
# Odločitve — fancontrol

## [2026-05-31] MCU: TZT ESP32-S3-N16R8
**Odločitev:** TZT ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM OPI)
**Razlog:** Dovolj flash za OTA + NVS brez LittleFS. PSRAM za graph buffer.
Enostaven prehod na Super Mini ko bo projekt stabilen.

## [2026-05-31] Web vmesnik: HTML v firmware stringu
**Odločitev:** buildPage() v webserver.cpp, brez LittleFS
**Razlog:** Enostavnejše, manj točk odpovedi. VAC_Plug dokazuje da deluje.
16MB flash daje dovolj prostora za velik HTML string.

## [2026-05-31] AsyncWebServer: me-no-dev
**Odločitev:** Ostanemo pri me-no-dev/ESPAsyncWebServer
**Razlog:** Deluje v vseh referenčnih projektih (VAC_Plug, vent_DEW, vent_SEW).
Ni razloga za tveganje z zamenjavo.

## [2026-05-31] Graf historia: PSRAM krožni buffer
**Odločitev:** ps_malloc() buffer v PSRAM, 60 točk = 60 minut, brez persistentnosti
**Razlog:** 8MB PSRAM je na voljo, preprost in zanesljiv. Po resetu buffer prazen — sprejemljivo.

## [2026-05-31] Referenčni projekt: vent_SEW
**Odločitev:** vent_SEW kot arhitekturni vzorec
**Razlog:** Identičen hardware tier (16MB+8MB), isti framework, ista knjižnjična stack.
Prevzamemo: globals vzorec, graph_store, web arhitekturo.
Ne prevzamemo: LVGL, kamera, SD, BME680, TCS, touch — balast ki ga ne rabimo.
```

---

## 13. Prevajanje in poročilo

Ko so vse datoteke ustvarjene:

1. Izvedi `pio run` v mapi `C:\PlatformIO\Projekti\fancontrol`
2. Prevajanje **mora** uspeti brez napak (warnings so OK)
3. Poročaj:
   - Seznam ustvarjenih datotek
   - Rezultat `pio run` (SUCCESS / FAILED)
   - Velikost firmware (Flash used / RAM used)
   - Seznam morebitnih warningov
   - Če je BUILD FAILED: pokaži napake in jih popravi, ponovi do SUCCESS

**Ne poročaj dokler ni `pio run` uspešen.**
