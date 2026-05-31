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
