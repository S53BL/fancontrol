// display.cpp — GxEPD2 WeAct 2.9" B/W, pokončna orientacija
#include "display.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "monitor.h"
#include "nanopi_client.h"
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>
#include <WiFi.h>
#include "wifi_config.h"
#include <qrcode.h>

// Font aliasi — tehno stil (_mf: Extended Latin, podpora za °C in Č)
#define FONT_LABEL  u8g2_font_profont12_mf
#define FONT_VALUE  u8g2_font_logisoso16_tf
#define FONT_TIME   u8g2_font_logisoso28_tf
#define FONT_DAY    u8g2_font_profont17_mf
#define FONT_UNIT   u8g2_font_profont12_mf

// DEPG0290BS, 128×296px, SSD1680 — pokončna orientacija
GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT> display(
    GxEPD2_290_BS(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// Centrira string horizontalno — font mora biti nastavljen pred klicem
static int16_t centerX(const char* str, uint8_t /*fontSize*/) {
    return (EPD_WIDTH - (int16_t)u8g2Fonts.getUTF8Width(str)) / 2;
}

bool initDisplay() {
    LOG_INFO("EPD", "initDisplay() — start");

    // GPIO pred SPI — preprečimo nedefiniran pin status
    pinMode(PIN_EPD_DC,   OUTPUT);
    pinMode(PIN_EPD_RST,  OUTPUT);
    pinMode(PIN_EPD_CS,   OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT_PULLUP);

    LOG_INFO("EPD", "GPIO OK, BUSY=%d", digitalRead(PIN_EPD_BUSY));

    SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
    delay(10);

    LOG_INFO("EPD", "SPI OK — kličem display.init()");

    // Reset pulse 50ms — kot demo EpaperModuleTest_Arduino_ESP32S3.ino
    // BUSY pre-check odstranjen — GxEPD2 opravi init sekvenco sam
    display.init(115200, true, 50, false);

    LOG_INFO("EPD", "display.init() OK, BUSY=%d", digitalRead(PIN_EPD_BUSY));

    display.setRotation(0);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());

    u8g2Fonts.begin(display);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

    LOG_INFO("EPD", "ePaper init OK");
    return true;
}

void showBootScreen() {
    if (sensorData.err & ERR_DISPLAY) return;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // ── Naziv in verzija (zgoraj) ─────────────────────────────
        u8g2Fonts.setFont(u8g2_font_profont12_mf);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setCursor(4, 14);
        u8g2Fonts.print("fancontrol");
        u8g2Fonts.setCursor(4, 28);
        u8g2Fonts.print(FW_VERSION);

        // ── Loading... (večji font) ───────────────────────────────
        u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
        u8g2Fonts.setCursor(4, 54);
        u8g2Fonts.print("Loading...");

        // ── Ločilna črta ─────────────────────────────────────────
        display.drawFastHLine(4, 62, 120, GxEPD_BLACK);

        // ── Mrežni podatki ────────────────────────────────────────
        u8g2Fonts.setFont(u8g2_font_profont12_mf);

        // ESP32 IP
        u8g2Fonts.setCursor(4, 76);
        u8g2Fonts.print(WIFI_STATIC_IP);

        // mDNS
        u8g2Fonts.setCursor(4, 90);
        u8g2Fonts.print(MDNS_HOSTNAME ".local");

        // Monitor IP (iz settings — nastavljiv v web vmesniku)
        u8g2Fonts.setCursor(4, 104);
        u8g2Fonts.print("PC: ");
        u8g2Fonts.print(settings.monitorIp);

        // ── QR koda — polna širina, spodaj ───────────────────────
        // QR vsebina: http://<IP>
        char qrStr[32];
        snprintf(qrStr, sizeof(qrStr), "http://%s", WIFI_STATIC_IP);

        QRCode qrcode;
        // Verzija 3 = 29x29 modulov — zadostuje za http://192.168.2.169
        uint8_t qrData[qrcode_getBufferSize(3)];
        qrcode_initText(&qrcode, qrData, 3, ECC_LOW, qrStr);

        // Izračun scale in pozicije — polna širina 128px
        // 29 modulov + 4 moduli quiet zone (vsaka stran) = 37 modulov
        // scale = floor(128 / 37) = 3 → 37*3 = 111px, centriramo
        const uint8_t qrScale    = 3;
        const uint8_t quietZone  = 4;
        const uint8_t qrModules  = qrcode.size + quietZone * 2;
        const uint8_t qrPx       = qrModules * qrScale;
        const int16_t qrX        = (EPD_WIDTH - qrPx) / 2;
        const int16_t qrY        = EPD_HEIGHT - qrPx - 2;  // 2px od spodnjega roba

        // Bela podlaga za QR (quiet zone)
        display.fillRect(qrX, qrY, qrPx, qrPx, GxEPD_WHITE);

        // Izriši module
        for (uint8_t row = 0; row < qrcode.size; row++) {
            for (uint8_t col = 0; col < qrcode.size; col++) {
                if (qrcode_getModule(&qrcode, col, row)) {
                    int16_t px = qrX + (quietZone + col) * qrScale;
                    int16_t py = qrY + (quietZone + row) * qrScale;
                    display.fillRect(px, py, qrScale, qrScale, GxEPD_BLACK);
                }
            }
        }

    } while (display.nextPage());

    LOG_INFO("EPD", "Boot screen OK");
}

static void drawFancyBar(int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t fillW) {
    const int16_t r = bh / 2;

    // Outline — pill shape (polkrožna konca)
    display.drawRoundRect(bx, by, bw, bh, r, GxEPD_BLACK);

    // Fill — levi polkrog + pravokotnik (+ desni polkrog pri polni vrednosti)
    if (fillW > 0) {
        int16_t fw = min(fillW, bw);
        // Levi polkrog (cel krog — desna half se pokrije s pravokotniki)
        display.fillCircle(bx + r, by + r, r, GxEPD_BLACK);
        // Telo pravokotnika desno od levega polkroga
        if (fw > r)
            display.fillRect(bx + r, by, fw - r, bh, GxEPD_BLACK);
        // Desni polkrog — samo ko fill doseže desni konec
        if (fw >= bw - r)
            display.fillCircle(bx + bw - r, by + r, r, GxEPD_BLACK);
        // Obreži desni rob fill — prepreči artefakt pri majhnih vrednostih
        if (fw < bw) {
            // Pobriši vse desno od fill do konca bara
            int16_t clearX = bx + fw;
            int16_t clearW = bw - fw;
            if (clearW > 0)
                display.fillRect(clearX, by + 1, clearW, bh - 2, GxEPD_WHITE);
        }
    }

    // Ponovi outline — popravi robove
    display.drawRoundRect(bx, by, bw, bh, r, GxEPD_BLACK);
}

// Dvojna vrh ikona — levi vrh nižji, desni višji
// x,y = levo spodaj ikone (baseline), skupna širina ~12px, višina 6px
static void drawPeakIcon(int16_t x, int16_t y) {
    // Levi vrh: baza 6px, višina 4px
    display.drawLine(x,   y, x+3, y-4, GxEPD_BLACK);
    display.drawLine(x+3, y-4, x+6, y, GxEPD_BLACK);
    // Desni vrh: baza 6px, višina 6px, offset +5px (1px razmak med njima)
    display.drawLine(x+5,  y, x+8, y-6, GxEPD_BLACK);
    display.drawLine(x+8, y-6, x+11, y, GxEPD_BLACK);
}

void updateDisplay(bool fullRefresh) {
    if (sensorData.err & ERR_DISPLAY) {
        LOG_WARN("EPD", "updateDisplay() preskocen — ERR_DISPLAY");
        return;
    }

    if (fullRefresh) {
        display.setFullWindow();
    } else {
        display.setPartialWindow(0, 0, EPD_WIDTH, EPD_HEIGHT);
    }
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // ── Ločilne črte ──────────────────────────────────────────
        display.drawFastHLine(4,  82, 120, GxEPD_BLACK);  // C1/C2
        display.drawFastHLine(4, 189, 120, GxEPD_BLACK);  // C2/C3
        // Spodnja črta C3 se NE riše — y=296 je izven zaslona (EPD_HEIGHT=296, veljavno 0–295)

        // ══════════════════════════════════════════════════════════
        // CONA 1 — Čas, sunrise/sunset, datum, zunanja temp/vlaga, wx ikona
        // y: 0–100
        // ══════════════════════════════════════════════════════════

        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

        // Ura (levo zgoraj)
        u8g2Fonts.setFont(u8g2_font_logisoso24_tf);
        u8g2Fonts.setCursor(4, 34);
        u8g2Fonts.print(timeSynced ? myTZ.dateTime("H:i").c_str() : "--:--");

        // Sunrise / Sunset (desno poravnano, samo HH:MM)
        u8g2Fonts.setFont(u8g2_font_profont12_mf);
        {
            const int16_t rEdge = EPD_WIDTH - 4;
            int16_t w = u8g2Fonts.getUTF8Width(weatherData.sunrise);
            u8g2Fonts.setCursor(rEdge - w, 22);
            u8g2Fonts.print(weatherData.sunrise);
            w = u8g2Fonts.getUTF8Width(weatherData.sunset);
            u8g2Fonts.setCursor(rEdge - w, 36);
            u8g2Fonts.print(weatherData.sunset);
        }

        // Datum: samo datum, brez imena dneva
        if (timeSynced) {
            char dateBuf[20];
            snprintf(dateBuf, sizeof(dateBuf), "%d.%d.%d",
                     myTZ.day(), myTZ.month(), myTZ.year());
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 51);
            u8g2Fonts.print(dateBuf);
        }

        // DND luna ikona (desno zgoraj, samo če aktiven)
        if (sensorData.dndActive) {
            display.fillCircle(113, 8, 7, GxEPD_BLACK);
            display.fillCircle(116, 5, 6, GxEPD_WHITE);
        }

        // Zunanja temperatura (levo)
        {
            char buf[12];
            if (!weatherData.valid) {
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 72);
                u8g2Fonts.print("--.-");
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(40, 72);
                u8g2Fonts.print("\xC2\xB0" "C");
            } else {
                snprintf(buf, sizeof(buf), "%.1f", weatherData.outTemp);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 72);
                u8g2Fonts.print(buf);
                int16_t tw = u8g2Fonts.getUTF8Width(buf);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(4 + tw + 2, 72);
                u8g2Fonts.print("\xC2\xB0" "C");
            }

        }

        // Wx ikona (desno, sc=6)
        if (weatherData.valid) {
            wxDrawConditionsScaled(92, 68, weatherData.wxCode, weatherData.isNight, 6, WX_ICON_LINESIZE);
        }

        // ══════════════════════════════════════════════════════════
        // CONA 2 — FanControl: temp+napetost, case fan, pc load
        // y: 84–187  (103px)
        // ══════════════════════════════════════════════════════════
        // PRERAČUN PRED IZPISOM:
        //   val   y=101: 88(zgornji rob)+13(ascent) = 101                ✓
        //   fan_label  y=116: 101+2(spodnji)+5(gap)+9 = 116              ✓
        //   fan_bar    y=118: 116+2 = 118                                ✓
        //   fan_val%   y=132: 118+14(bar_h) = 132                        ✓
        //   fan_peak   y=143: 132+2+9 = 143                              ✓
        //   load_label y=158: 143+2(spodnji)+4(gap)+9 = 158              ✓
        //   load_bar   y=160: 158+2 = 160                                ✓
        //   load_val   y=174: 160+14 = 174                               ✓
        //   load_peak  y=185: 174+2+9 = 185                              ✓
        //   C2 konec:   185+2 = 187  → črta y=189                       ✓
        {
            bool shtErr = (sensorData.err & ERR_SHT30);
            bool inaErr = (sensorData.err & ERR_INA219);
            char buf[12];

            // ── Vrednosti logo16 (brez labelov) — y=101 ───────────────────
            // Temperatura
            if (shtErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.temp);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(4, 101); u8g2Fonts.print(buf);
            int16_t tw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4 + tw + 1, 101); u8g2Fonts.print("\xC2\xB0" "C");

            // Napetost
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.volt);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(68, 101); u8g2Fonts.print(buf);
            int16_t vw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(68 + vw + 1, 101); u8g2Fonts.print("V");

            // ── CASE FAN bar — label y=116, bar y=118–132 ─────────────────
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 116); u8g2Fonts.print("CASE FAN");
            {
                const int16_t bx=4, by=118, bw=84, bh=14;
                int16_t fill = (int16_t)((int32_t)bw * sensorData.fanPct / 100);
                drawFancyBar(bx, by, bw, bh, fill);
                char pBuf[6];
                snprintf(pBuf, sizeof(pBuf), "%d%%", sensorData.fanPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                int16_t vx = 124 - (int16_t)u8g2Fonts.getUTF8Width(pBuf);
                if (vx < 90) vx = 90;  // ne sme se prekrivati z barom
                u8g2Fonts.setCursor(vx, 132); u8g2Fonts.print(pBuf);
            }
            // Peak ikona + vrednost — y=143
            {
                drawPeakIcon(4, 143);
                char pk[8];
                snprintf(pk, sizeof(pk), " %d%%", peakFan);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(17, 143); u8g2Fonts.print(pk);
            }

            // ── PC LOAD bar — label y=158, bar y=160–174 ──────────────────
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 158); u8g2Fonts.print("PC LOAD");
            {
                const int16_t bx=4, by=160, bw=84, bh=14;
                float peak = sensorData.peakWatt;
                if (peak < PEAK_WATT_MIN_FLOOR) peak = PEAK_WATT_DEFAULT;
                float ratio = inaErr ? 0.0f : (sensorData.watt / peak);
                if (ratio > 1.0f) ratio = 1.0f;
                int16_t fill = (int16_t)(bw * ratio);
                drawFancyBar(bx, by, bw, bh, fill);
                {
                    char wBuf[8];
                    snprintf(wBuf, sizeof(wBuf), "%dW", (int)roundf(sensorData.watt));
                    const char* wStr = inaErr ? "--W" : wBuf;
                    u8g2Fonts.setFont(inaErr ? u8g2_font_profont12_mf : u8g2_font_logisoso16_tf);
                    int16_t vx = 124 - (int16_t)u8g2Fonts.getUTF8Width(wStr);
                    if (vx < 90) vx = 90;
                    u8g2Fonts.setCursor(vx, 174); u8g2Fonts.print(wStr);
                }
            }
            // Peak ikona + vrednost — y=185
            {
                drawPeakIcon(4, 185);
                char pk[10];
                float p = sensorData.peakWatt;
                if (p < PEAK_WATT_MIN_FLOOR) p = PEAK_WATT_DEFAULT;
                snprintf(pk, sizeof(pk), " %dW", (int)roundf(p));
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(17, 185); u8g2Fonts.print(pk);
            }
        }

        // ══════════════════════════════════════════════════════════
        // CONA 3 — NanoPi: status, traffic, attacks
        // y: 191–289  (98px)
        // ══════════════════════════════════════════════════════════
        // PRERAČUN PRED IZPISOM:
        //   status text  y=200: 191+9(gap+ascent) = 200                  ✓
        //   trf_label  y=214: 200+3(spodnji)+2(gap)+9 = 214              ✓
        //   trf_bar    y=216: 214+2 = 216                                ✓
        //   trf_val%   y=230: 216+14(bar_h) = 230                        ✓
        //   trf_sub    y=245: 230+2+13(ascent logo16) = 245              ✓
        //   atk_label  y=258: 245+3(spodnji)+1(gap)+9 = 258              ✓
        //   atk_bar    y=260: 258+2 = 260                                ✓
        //   atk_val%   y=274: 260+14 = 274                               ✓
        //   atk_sub    y=289: 274+2+13 = 289  ← zadnji element           ✓
        // ══════════════════════════════════════════════════════════
        {
            NanoPiData nd = nanopiGetData();

            // Status: izpiše label, ob napaki nariše diagonalno črto čezenj
            auto drawStatusLabel = [&](int16_t tx, int16_t ty, const char* label, bool ok) {
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(tx, ty);
                u8g2Fonts.print(label);
                if (!ok) {
                    int16_t tw = (int16_t)u8g2Fonts.getUTF8Width(label);
                    // Diagonala od levega spodnjega do desnega zgornjega roba besedila
                    display.drawLine(tx, ty + 1, tx + tw, ty - 9, GxEPD_BLACK);
                }
            };

            // ── Status vrstica: SVC + banIP — text y=200 ──────────────────
            drawStatusLabel(4,  200, "SVC",   nd.valid && nd.serverOk);
            drawStatusLabel(68, 200, "banIP", nd.valid && nd.banipRunning);

            // ── TRAFFIC bar — label y=214, bar y=216–230, sub y=245 ───────
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 214); u8g2Fonts.print("TRAFFIC");

            {
                const int16_t bx = 4, by = 216, bw = 84, bh = 14;
                // Skala 0–120%: 84px = 120%, fill clamped na bw
                uint8_t barPct  = nd.valid ? nd.networkBarPct : 0;
                int16_t fill    = (int16_t)min((int32_t)bw * barPct / 120, (int32_t)bw);
                drawFancyBar(bx, by, bw, bh, fill);

                // 100% tick = pikčasta navpična črta pri x=74 (=4 + 84*100/120)
                uint16_t tickCol = (fill > 70) ? GxEPD_WHITE : GxEPD_BLACK;
                for (int16_t ty = by + 2; ty < by + bh - 2; ty += 2) {
                    display.drawPixel(74, ty, tickCol);
                }

                char pBuf[6];
                snprintf(pBuf, sizeof(pBuf), "%d%%", barPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                int16_t vx = 124 - (int16_t)u8g2Fonts.getUTF8Width(pBuf);
                if (vx < 90) vx = 90;
                u8g2Fonts.setCursor(vx, 230); u8g2Fonts.print(pBuf);
            }

            // Pod barom: vrednost + enota + peak ikona + peak vrednost — y=245
            {
                char curBuf[8];
                if (!nd.valid) snprintf(curBuf, sizeof(curBuf), "--");
                else           snprintf(curBuf, sizeof(curBuf), "%.2f", nd.networkCurrentMbits);

                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 245); u8g2Fonts.print(curBuf);
                int16_t xa = 4 + u8g2Fonts.getUTF8Width(curBuf);

                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                const char* unit = " Mb/s";
                u8g2Fonts.setCursor(xa, 245); u8g2Fonts.print(unit);
                int16_t xb = xa + u8g2Fonts.getUTF8Width(unit) + 4;

                drawPeakIcon(xb, 245);
                char pkBuf[10];
                if (!nd.valid) snprintf(pkBuf, sizeof(pkBuf), " --");
                else           snprintf(pkBuf, sizeof(pkBuf), " %.2f", nd.networkPeakMbits);
                u8g2Fonts.setCursor(xb + 13, 245); u8g2Fonts.print(pkBuf);
            }

            // ── ATTACKS bar — label y=258, bar y=260–274, sub y=289 ───────
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 258); u8g2Fonts.print("ATTACKS");

            {
                const int16_t bx = 4, by = 260, bw = 84, bh = 14;
                uint8_t barPct  = nd.valid ? nd.attacksBarPct : 0;
                int16_t fill    = (int16_t)min((int32_t)bw * barPct / 120, (int32_t)bw);
                drawFancyBar(bx, by, bw, bh, fill);

                uint16_t tickCol = (fill > 70) ? GxEPD_WHITE : GxEPD_BLACK;
                for (int16_t ty = by + 2; ty < by + bh - 2; ty += 2) {
                    display.drawPixel(74, ty, tickCol);
                }

                char pBuf[6];
                snprintf(pBuf, sizeof(pBuf), "%d%%", barPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                int16_t vx = 124 - (int16_t)u8g2Fonts.getUTF8Width(pBuf);
                if (vx < 90) vx = 90;
                u8g2Fonts.setCursor(vx, 274); u8g2Fonts.print(pBuf);
            }

            // Pod barom: vrednost + enota + peak ikona + peak vrednost — y=289 (zadnji element)
            {
                char curBuf[8];
                if (!nd.valid) snprintf(curBuf, sizeof(curBuf), "--");
                else           snprintf(curBuf, sizeof(curBuf), "%lu", (unsigned long)nd.attacksCurrentPh);

                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 289); u8g2Fonts.print(curBuf);
                int16_t xa = 4 + u8g2Fonts.getUTF8Width(curBuf);

                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                const char* unit = " /h";
                u8g2Fonts.setCursor(xa, 289); u8g2Fonts.print(unit);
                int16_t xb = xa + u8g2Fonts.getUTF8Width(unit) + 4;

                drawPeakIcon(xb, 289);
                char pkBuf[12];
                if (!nd.valid) snprintf(pkBuf, sizeof(pkBuf), " --");
                else           snprintf(pkBuf, sizeof(pkBuf), " %lu", (unsigned long)nd.attacksPeakPh);
                u8g2Fonts.setCursor(xb + 13, 289); u8g2Fonts.print(pkBuf);
            }

            // stale indikator — invertiran kvadratek v zgornjem desnem kotu Cone 3
            if (nd.stale && nd.valid) {
                display.fillRect(122, 191, 6, 6, GxEPD_BLACK);
            }
        }

    } while (display.nextPage());
}

// ── GxEPD2 API aliasi za LilyGo-style ikona primitivi ─────────────────────
#define fillCircle(x,y,r,c)       display.fillCircle(x,y,r,c)
#define drawCircle(x,y,r,c)       display.drawCircle(x,y,r,c)
#define fillRect(x,y,w,h,c)       display.fillRect(x,y,w,h,c)
#define drawLine(x0,y0,x1,y1,c)  display.drawLine(x0,y0,x1,y1,c)
#define drawPixel(x,y,c)          display.drawPixel(x,y,c)

// ── Ikona primitivi (adaptirani iz G6EJD LilyGo projekta, scale=WX_ICON_SCALE) ──

static void wxAddCloud(int x, int y, int scale, int linesize) {
    fillCircle(x - scale * 3, y,               scale,               GxEPD_BLACK);
    fillCircle(x + scale * 3, y,               scale,               GxEPD_BLACK);
    fillCircle(x - scale,     y - scale,       scale * 1.4,         GxEPD_BLACK);
    fillCircle(x + scale*1.5, y - scale*1.3,   scale * 1.75,        GxEPD_BLACK);
    fillRect  (x - scale*3-1, y - scale,       scale*6,  scale*2+1, GxEPD_BLACK);
    fillCircle(x - scale * 3, y,               scale - linesize,    GxEPD_WHITE);
    fillCircle(x + scale * 3, y,               scale - linesize,    GxEPD_WHITE);
    fillCircle(x - scale,     y - scale,       scale*1.4-linesize,  GxEPD_WHITE);
    fillCircle(x + scale*1.5, y - scale*1.3,   scale*1.75-linesize, GxEPD_WHITE);
    fillRect  (x - scale*3+2, y-scale+linesize-1, scale*5.9, scale*2-linesize*2+2, GxEPD_WHITE);
}

static void wxAddSun(int x, int y, int scale) {
    int linesize = WX_ICON_LINESIZE + 1;
    fillRect(x - scale*2, y,           scale*4,   linesize, GxEPD_BLACK);
    fillRect(x,           y - scale*2, linesize,  scale*4,  GxEPD_BLACK);
    drawLine(x+scale*1.4, y+scale*1.4, x-scale*1.4, y-scale*1.4, GxEPD_BLACK);
    drawLine(x-scale*1.4, y+scale*1.4, x+scale*1.4, y-scale*1.4, GxEPD_BLACK);
    fillCircle(x, y, scale * 1.3, GxEPD_WHITE);
    fillCircle(x, y, scale,       GxEPD_BLACK);
    fillCircle(x, y, scale - linesize, GxEPD_WHITE);
}

static void wxAddMoon(int x, int y) {
    fillCircle(x,   y,   9, GxEPD_BLACK);
    fillCircle(x+4, y-3, 7, GxEPD_WHITE);
}

static void wxAddRain(int x, int y, int scale) {
    for (int i = 0; i < 4; i++) {
        drawLine(x - scale*2 + i*scale,     y + scale*2,
                 x - scale*2 + i*scale - 2, y + scale*3, GxEPD_BLACK);
    }
}

static void wxAddSnow(int x, int y, int scale) {
    for (int i = 0; i < 4; i++) {
        int sx = x - scale*2 + i*(scale + 1);
        int sy = y + scale*2 + 2;
        drawLine(sx-2, sy, sx+2, sy, GxEPD_BLACK);
        drawLine(sx, sy-2, sx, sy+2, GxEPD_BLACK);
    }
}

static void wxAddTstorm(int x, int y, int scale) {
    int bx = x - scale;
    drawLine(bx,       y,            bx+scale, y+scale*1.5, GxEPD_BLACK);
    drawLine(bx+scale, y+scale*1.5,  bx,       y+scale*1.5, GxEPD_BLACK);
    drawLine(bx,       y+scale*1.5,  bx+scale, y+scale*3,   GxEPD_BLACK);
}

static void wxAddFog(int x, int y, int scale) {
    for (int i = 0; i < 3; i++) {
        fillRect(x - scale*3, y + scale*(i+1), scale*6, WX_ICON_LINESIZE, GxEPD_BLACK);
    }
}

// ── WMO weather_code → ikona z eksplicitnim scale parametrom ────────────
void wxDrawConditionsScaled(int x, int y, uint8_t wxCode, bool isNight, int sc, int ls) {
    if (wxCode == 0) {
        if (isNight) wxAddMoon(x, y);
        else         wxAddSun(x, y, sc);

    } else if (wxCode <= 2) {
        if (isNight) wxAddMoon(x - sc*2, y - sc*2);
        else         wxAddSun(x - sc*2,  y - sc*2, sc - 2);
        wxAddCloud(x, y, (int)(sc * 0.8f), ls);

    } else if (wxCode == 3) {
        wxAddCloud(x - sc, y - sc, sc/2, ls);
        wxAddCloud(x,      y,      sc,   ls);

    } else if (wxCode == 45 || wxCode == 48) {
        wxAddFog(x, y, sc);

    } else if (wxCode >= 51 && wxCode <= 67) {
        wxAddCloud(x, y - sc/2, sc, ls);
        wxAddRain(x, y - sc/2, sc);

    } else if (wxCode >= 71 && wxCode <= 77) {
        wxAddCloud(x, y - sc/2, sc, ls);
        wxAddSnow(x, y - sc/2, sc);

    } else if (wxCode >= 80 && wxCode <= 82) {
        wxAddSun(x - sc*2, y - sc*2, sc - 2);
        wxAddCloud(x, y - sc/2, sc, ls);
        wxAddRain(x, y - sc/2, sc);

    } else if (wxCode >= 85 && wxCode <= 86) {
        wxAddCloud(x, y - sc/2, sc, ls);
        wxAddSnow(x, y - sc/2, sc);

    } else if (wxCode >= 95 && wxCode <= 99) {
        wxAddCloud(x, y - sc/2, sc, ls);
        wxAddTstorm(x, y - sc/2, sc);

    } else {
        u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
        u8g2Fonts.setCursor(x - 5, y + 8);
        u8g2Fonts.print("?");
    }
}

// ── WMO weather_code → ikona (9 skupin) ──────────────────────────────────
void wxDrawConditions(int x, int y, uint8_t wxCode, bool isNight) {
    const int sc = WX_ICON_SCALE;
    const int ls = WX_ICON_LINESIZE;

    if (wxCode == 0) {
        if (isNight) wxAddMoon(x, y);
        else         wxAddSun(x, y, sc);

    } else if (wxCode <= 2) {
        if (isNight) wxAddMoon(x - sc*3, y - sc*2);
        wxAddSun(x - sc*2, y - sc*2, sc);
        wxAddCloud(x, y, sc * 0.9, ls);

    } else if (wxCode == 3) {
        wxAddCloud(x - sc*2, y - sc*2, sc/2, ls);
        wxAddCloud(x,        y,        sc,   ls);

    } else if (wxCode == 45 || wxCode == 48) {
        wxAddFog(x, y, sc);

    } else if (wxCode >= 51 && wxCode <= 67) {
        wxAddCloud(x, y, sc, ls);
        wxAddRain(x, y, sc);

    } else if (wxCode >= 71 && wxCode <= 77) {
        wxAddCloud(x, y, sc, ls);
        wxAddSnow(x, y, sc);

    } else if (wxCode >= 80 && wxCode <= 82) {
        wxAddSun(x - sc*2, y - sc*2, sc);
        wxAddCloud(x, y, sc, ls);
        wxAddRain(x, y, sc);

    } else if (wxCode >= 85 && wxCode <= 86) {
        wxAddCloud(x, y, sc, ls);
        wxAddSnow(x, y, sc);

    } else if (wxCode >= 95 && wxCode <= 99) {
        wxAddCloud(x, y, sc, ls);
        wxAddTstorm(x, y, sc);

    } else {
        u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
        u8g2Fonts.setCursor(x - 5, y + 8);
        u8g2Fonts.print("?");
    }
}
