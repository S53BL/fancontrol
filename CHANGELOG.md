# Changelog — fancontrol

## [0.5.0] — 2026-05-31

### Dodano

- logging.h/cpp: RAM log buffer (200 vnosov, PSRAM), makri LOG_INFO/WARN/ERROR, hh:mm:ss timestamp
- webserver.cpp: popolna implementacija — WiFi statični IP (192.168.2.169), NTP, mDNS
- webserver.cpp: single-page app — dark tehno tema, 4 tabi (Dashboard/Ventilator/Kalibracija/Sistem)
- webserver.cpp: Dashboard — 5 live kartic z peak trackerjem, 3 Chart.js grafi
- webserver.cpp: API endpointi — /api/data, /api/history, /api/log, /save/fan, /save/cal
- webserver.cpp: OTA flash (Update.h, identično vent_SEW vzorcu)
- globals: peakTemp, peakWatt peak tracker spremenljivki
- config.h: STATIC_IP, MDNS_HOSTNAME, FW_VERSION, LOG_BUFFER_ENTRIES konstante
- globals/Settings: tempOffset, humOffset, shuntOhms, currentCorr, fanMaxDayPct
- sensors.cpp: kalibracija offsetov pri branju SHT30 in INA219
- main.cpp: logInit() kot prvi klic v setup(), peak tracker v loop()

## [0.3.0] — 2026-05-31

- graph_store.cpp: dokončan krožni PSRAM buffer, thread-safe graphAddPoint/graphGetCount
- display.cpp: GxEPD2_290_BS init (WeAct 2.9" B/W, SSD1680), SPI2 na ESP32-S3
- display.cpp: U8g2_for_Adafruit_GFX integracija, tehno font stack (logisoso, profont)
- display.cpp: 4-conski layout (čas/dan, temp/vlaga, V/A/W, fan progress bar + DND)
- display.h: odstranjen partialDisplay() skeleton
- platformio.ini: dodana olikraus/U8g2_for_Adafruit_GFX knjižnjica
- display.cpp: fonti popravljeni `_mr` → `_mf` (Extended Latin: podpora za °C in Č)

## [0.2.0] — 2026-05-31

- sensors.cpp: SHT30 branje (temp, vlaga) z validacijo in ERR flagom
- sensors.cpp: INA219 branje (V, A, W) z validacijo in ERR flagom
- sensors.cpp: thread-safe pisanje v sensorData (dataMux)
- fan.cpp: PWM init (LEDC), setFanPct() z minimum zaščito
- fan.cpp: isDndActive() z ezTime, podpora za čez-polnočni interval
- fan.cpp: updateFan() s linearno interpolacijo temperaturne krivulje + DND korekcija

## [0.1.1] — 2026-05-31

- .gitignore: razširjen (dodano .claude/, .vscode/, Doc/, AGENT_*.md, wifi_config.h, build artefakti)
- .gitignore: odstranjeno sledenje že-trackanim mapam (.claude/, .vscode/, Doc/, AGENT_*.md, to-do.md, decisions.md)
- platformio.ini: odstranjen [env:esp32s3_debug] — samo en build environment

## [0.1.0] — 2026-05-31

- Skelet projekta: platformio.ini, particijska tabela, vse src/ datoteke
- config.h: pinout TZT ESP32-S3-N16R8, konstante, enumi
- globals.h/cpp: SensorData, Settings, NVS load/save z CRC16
- sensors.h/cpp: skeleton SHT30 + INA219
- fan.h/cpp: skeleton PWM krmiljenje, temperaturna krivulja
- display.h/cpp: skeleton GxEPD2 WeAct 2.9" B/W
- webserver.h/cpp: skeleton AsyncWebServer, buildPage(), API endpointi
- graph_store.h/cpp: krožni PSRAM buffer, GraphPoint struktura
- main.cpp: setup/loop skeleton, timing logika
