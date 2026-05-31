# Changelog — fancontrol

## [0.2.0] — 2026-05-31

### Implementirano

- sensors.cpp: SHT30 branje (temp, vlaga) z validacijo in ERR flagom
- sensors.cpp: INA219 branje (V, A, W) z validacijo in ERR flagom
- sensors.cpp: thread-safe pisanje v sensorData (dataMux)
- fan.cpp: PWM init (LEDC), setFanPct() z minimum zaščito
- fan.cpp: isDndActive() z ezTime, podpora za čez-polnočni interval
- fan.cpp: updateFan() s linearno interpolacijo temperaturne krivulje + DND korekcija

## [0.1.1] — 2026-05-31

### Spremenjeno

- .gitignore: razširjen (dodano .claude/, .vscode/, Doc/, AGENT_*.md, wifi_config.h, build artefakti)
- .gitignore: odstranjeno sledenje že-trackanim mapam (.claude/, .vscode/, Doc/, AGENT_*.md, to-do.md, decisions.md)
- platformio.ini: odstranjen [env:esp32s3_debug] — samo en build environment

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
