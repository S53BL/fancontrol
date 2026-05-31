# Pametni ventilator za ACEMAGIC S1 Mini PC
**Idejni projekt — Verzija 2.0**
**Datum: 31. maj 2026**

---

## 1. Namen projekta

Izdelava pametnega avtomatskega ventilatorja za hlajenje ACEMAGIC S1 Mini PC (Intel Alder Lake N100). Naprava samostojno meri temperaturo okolice in vlažnost, prilagaja hitrost ventilatorja po nastavljivi krivulji, meri porabo 12V napajanja Mini PC ter omogoča daljinski nadzor in vizualizacijo podatkov z grafi preko lokalnega web vmesnika. Na 2.9" ePaper zaslonu je vedno viden lokalni prikaz ključnih podatkov.

Rešitev deluje neodvisno, se poveže na obstoječe domače omrežje (WiFi client), sinhronizira čas preko NTP in podpira OTA posodobitve firmware.

---

## 2. Hardware

| Komponenta | Model / Specifikacija | Opomba |
|---|---|---|
| Mikrokontroler | TZT ESP32-S3-N16R8 | Dual-core LX7, 240MHz, 16MB Flash, 8MB PSRAM OPI |
| Temperaturni in vlažnostni senzor | SHT30 (I2C) | ±0.2°C natančnost |
| Merjenje napetosti in toka | INA219 (I2C) | Meri 12V vejo Mini PC (V, A, W) |
| Zaslon | WeAct Studio 2.9" ePaper B/W | 296×128 px, SPI, pokončna orientacija, partial refresh |
| Ventilator | 12V axial, 2-pin (80 mm ali 92 mm) | Tihi model, npr. Sunon ali Noctua |
| Krmiljenje ventilatorja | LR7843 MOSFET trigger modul | PWM signal iz ESP32, logic level 3.3V |
| Napajanje | 12V / 1.5–2A adapter | En vir za vse |
| DC-DC pretvornik | Mini360 PRO (12V → 5V) | Napaja ESP32 in periferne module |
| Ohišje / pritrditev | 3D tiskan nosilec | Po meri za ACEMAGIC S1 |

---

## 3. Napajalna arhitektura

```
12V adapter
    │
    ├──► Ventilator (via LR7843 MOSFET modul)
    │
    ├──► Mini360 PRO DC-DC (12V → 5V)
    │        └──► ESP32-S3 (5V USB pin)
    │             └──► SHT30, INA219, ePaper (3.3V iz ESP32 LDO)
    │
    └──► [INA219 shunt] ──► ACEMAGIC S1 Mini PC (12V)
```

**Merjenje INA219:** samo 12V veja Mini PC. Ventilator in ESP32 napajanje sta na ločeni veji brez merjenja.

---

## 4. Pinout — TZT ESP32-S3-N16R8

### I2C bus (SHT30 + INA219)

| Signal | GPIO | Opomba |
|---|---|---|
| SDA | GPIO 8 | I2C data, oba modula na istem busu |
| SCL | GPIO 9 | I2C clock |

**I2C naslovi:**
- SHT30: `0x44` (privzeto)
- INA219: `0x40` (privzeto)

### SPI bus — ePaper WeAct 2.9" B/W

| Signal | GPIO | Opomba |
|---|---|---|
| MOSI (DIN) | GPIO 11 | SPI data |
| CLK (SCK) | GPIO 12 | SPI clock |
| CS | GPIO 10 | Chip select (aktiven LOW) |
| DC | GPIO 13 | Data/Command |
| RST | GPIO 14 | Reset (aktiven LOW) |
| BUSY | GPIO 15 | Busy signal (čakanje na refresh) |

### Ventilator PWM

| Signal | GPIO | Opomba |
|---|---|---|
| PWM → LR7843 gate | GPIO 5 | PWM ~1kHz, 0–100% duty cycle |

### Napajanje

| Pin | Napetost | Opomba |
|---|---|---|
| 5V (USB pin) | 5V | Iz Mini360 PRO |
| GND | 0V | Skupna masa |

> **Opomba:** GPIO 8/9 so privzeti I2C pini na ESP32-S3. GPIO 11/12 so standardni SPI2 pini. Vse napetosti signalov so 3.3V — kompatibilno z vsemi moduli.

---

## 5. Programska arhitektura

### Platforma
- **PlatformIO** + Arduino framework
- **Board:** `esp32-s3-devkitc-1`
- **Flash:** 16MB QIO, **PSRAM:** 8MB OPI
- **Particijska tabela:** custom 16MB OTA + NVS (brez LittleFS)

### Struktura modulov

```
src/
├── main.cpp          — setup(), loop(), inicializacija, multi-core task razporeditev
├── config.h          — vsi pinout defini, konstante, privzete vrednosti
├── globals.h/cpp     — deljene globalne spremenljivke, mutex zaščita
├── sensors.cpp/h     — branje SHT30 (temp, vlaga) + INA219 (V, A, W)
├── fan.cpp/h         — PWM krmiljenje ventilatorja, temperaturna krivulja, DND način
├── display.cpp/h     — GxEPD2 driver, izris zaslona, partial refresh logika
├── webserver.cpp/h   — AsyncWebServer, HTML v firmware stringu, /api/data JSON endpoint
└── graph_store.cpp/h — krožni buffer za zgodovino meritev (PSRAM), JSON za grafe
```

### Knjižnjice (platformio.ini lib_deps)

```ini
me-no-dev/AsyncTCP
me-no-dev/ESPAsyncWebServer
bblanchon/ArduinoJson @ ^6.21.0
ropg/ezTime
zinggjm/GxEPD2 @ ^1.6.0
adafruit/Adafruit SHT31 Library @ ^2.2.2   ; SHT30 kompatibilna
adafruit/Adafruit INA219 @ ^1.2.3
adafruit/Adafruit BusIO @ ^1.14.5
```

### Multi-core razporeditev

| Core | Naloga |
|---|---|
| Core 0 | WiFi stack, AsyncWebServer handler |
| Core 1 | Senzorsko branje, PWM krmiljenje, ePaper osvežitev (loop) |

---

## 6. Funkcionalnosti

### 6.1 Krmiljenje ventilatorja
- PWM regulacija hitrosti (0–100%) via LR7843 MOSFET modul
- Temperaturna krivulja: nastavljiva po točkah (npr. pod 35°C = 20%, 45°C = 50%, 55°C = 80%, nad 60°C = 100%)
- Minimalna hitrost nastavljiva (preprečevanje popolnega izklopa = zaščita pred pregrevano lopo)
- **DND / Nočni tihi način:** časovno okno (npr. 22:00–07:00), maksimalna hitrost omejena na nastavljiv %

### 6.2 Merjenje
- **SHT30:** temperatura (°C) in relativna vlažnost (%)
- **INA219:** napetost (V), tok (A), moč (W) — samo Mini PC veja

### 6.3 ePaper zaslon (pokončna orientacija, 128×296 px)
Osvežitev na vsako minuto (full refresh ~3s) ali ob večji spremembi temperature (partial refresh).

**Postavitev zaslona (pokončno, 4 cone):**
```
┌──────────────────┐
│  🕐  14:35  pet  │  ← čas in dan (NTP)
├──────────────────┤
│  Temp:  42.3 °C  │
│  Vlaga: 38 %     │  ← senzorski podatki
├──────────────────┤
│  12.1V  0.85A    │
│  10.3W           │  ← Mini PC poraba
├──────────────────┤
│  Fan:  ████░ 64% │  ← hitrost ventilatorja (progress bar)
│  DND:  OFF       │
└──────────────────┘
```

### 6.4 Web vmesnik
Dostopen na lokalnem omrežju (`http://[IP]` ali `http://fancontrol.local`).

**Struktura strani (single-page, tab navigacija):**

**Tab 1 — Dashboard (monitoring)**
- Live podatki: temperatura, vlaga, V/A/W, hitrost ventilatorja (%)
- Graf temperature — zadnjih 60 minut (Chart.js, vgrajen v firmware)
- Graf porabe W — zadnjih 60 minut
- Graf hitrosti ventilatorja % — zadnjih 60 minut
- Avto-refresh podatkov vsakih 5 sekund via `/api/data` JSON endpoint (brez reload strani)

**Tab 2 — Nastavitve**
- Temperaturna krivulja: 4 nastavljive točke (temp → % PWM)
- DND način: enable/disable + čas od/do
- Minimalna hitrost ventilatorja (%)
- WiFi SSID / geslo (za primer menjave omrežja)
- Shranjevanje v NVS (Preferences)

**Tab 3 — Sistem**
- Verzija firmware, uptime, IP naslov, RSSI
- OTA posodobitev firmware (upload .bin)

### 6.5 Shranjevanje zgodovine (graph_store)
- **Krožni buffer v PSRAM** (8MB na voljo — ni problema)
- Vzorčenje vsakih 60 sekund → 60 točk = 1 ura zgodovine
- Shranjeni podatki: timestamp, temp, vlaga, V, A, W, fan%
- Po resetu: buffer prazen (ni persistentne shrambe zgodovine — sprejemljivo)

### 6.6 NTP in čas
- ezTime knjižnjica
- Časovna cona: `Europe/Ljubljana`
- Čas viden na ePaper zaslonu in v web vmesniku

### 6.7 OTA posodobitve
- ElegantOTA ali ArduinoOTA — odprto za kasnejšo odločitev
- Dostopno iz web vmesnika (Tab 3)

---

## 7. Web vmesnik — tehnični opis

### Pristop: HTML vgrajen v firmware (brez LittleFS)
Inspiriran z VAC_Plug arhitekturo — `buildPage()` funkcija vrne celoten HTML string.

**Grafi:** Chart.js knjižnjica naložena iz CDN (`https://cdn.jsdelivr.net/npm/chart.js`) ali vgrajena kot PROGMEM header (za offline delovanje). Odločitev pri implementaciji.

**API endpoint:**
```
GET /api/data
→ JSON: { "temp": 42.3, "hum": 38, "volt": 12.1, "amp": 0.85, "watt": 10.3,
          "fan": 64, "dnd": false, "time": "14:35", "uptime": 3600 }

GET /api/history
→ JSON array: [{ "ts": 1234567, "temp": 42.1, "watt": 10.1, "fan": 62 }, ...]

POST /save
→ form data: nastavitve krivulje, DND, itd.
```

### Thread safety
Mutex zaščita (portMUX) za vse deljene spremenljivke med Core 0 (web handler) in Core 1 (senzorji/fan) — prevzeto iz VAC_Plug vzorca.

---

## 8. Referenčni projekti

| Projekt | Kaj prevzamemo |
|---|---|
| **VAC_Plug** (S53BL) | Arhitektura webserver.cpp, buildPage(), Preferences NVS, mutex vzorec, /save POST |
| **vent_DEW** (S53BL) | platformio.ini osnova za ESP32-S3, ezTime integracija, ArduinoJson |
| **vent_SEW** (S53BL) | graph_store arhitektura (krožni buffer), web_handlers ločitev, 16MB+8MB PSRAM config |
| **WeActStudio.EpaperModule** | GxEPD2 inicializacija in primeri za 2.9" B/W panel |

---

## 9. Odprte odločitve (za kasnejšo fazo)

| # | Vprašanje | Možnosti |
|---|---|---|
| 1 | Chart.js: CDN ali PROGMEM? | CDN = vedno aktualno, zahteva internet; PROGMEM = offline, večji firmware |
| 2 | OTA knjižnjica | ArduinoOTA (CLI) ali ElegantOTA (web UI) |
| 3 | mDNS hostname | `fancontrol.local` — implementirati z ESPmDNS |
| 4 | Alarm pri visoki temp | Samo vizualno (ePaper + web) ali tudi MQTT/HTTP webhook? |
| 5 | WiFi fallback | AP mode + captive portal če STA ne uspe? |

---

## 10. Projektna struktura datotek (PlatformIO)

```
fan_controller_s1/
├── platformio.ini
├── partitions_16mb_ota.csv
├── CHANGELOG.md
├── to-do.md
├── decisions.md
└── src/
    ├── main.cpp
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
    └── graph_store.cpp
```

---

*Dokument je osnova za odprtje PlatformIO projekta. Vse odprte odločitve se razrešijo sproti med implementacijo.*
