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

// Vrne slovensko ime dneva — cela beseda brez šumnikov
static const char* getDaySLO() {
    if (!timeSynced) return "---";
    String d = myTZ.dateTime("D");
    if (d == "Mon") return "Ponedeljek";
    if (d == "Tue") return "Torek";
    if (d == "Wed") return "Sreda";
    if (d == "Thu") return "Cetrtek";
    if (d == "Fri") return "Petek";
    if (d == "Sat") return "Sobota";
    if (d == "Sun") return "Nedelja";
    return "---";
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
        // Obreži desni "štrleči" del kroga — ravna desna meja fill-a
        if (fw < 2 * r)
            display.fillRect(bx + fw, by, 2 * r - fw, bh, GxEPD_WHITE);
    }

    // Ponovi outline — popravi robove
    display.drawRoundRect(bx, by, bw, bh, r, GxEPD_BLACK);
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
        display.drawFastHLine(4,  65, 120, GxEPD_BLACK);  // C1/C2
        display.drawFastHLine(4, 194, 120, GxEPD_BLACK);  // C2/C3
        display.drawFastHLine(4, 295, 120, GxEPD_BLACK);  // spodnja

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

        // Datum: cela beseda + datum
        if (timeSynced) {
            char dateBuf[32];
            snprintf(dateBuf, sizeof(dateBuf), "%s %d.%d.%d",
                     getDaySLO(), myTZ.day(), myTZ.month(), myTZ.year());
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

            // Zunanja vlaga — logisoso16 (rahlo pod temp)
            if (!weatherData.valid) {
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 94);
                u8g2Fonts.print("--");
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(22, 94);
                u8g2Fonts.print("%");
            } else {
                snprintf(buf, sizeof(buf), "%d", (int)weatherData.outHum);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 94);
                u8g2Fonts.print(buf);
                int16_t hw = u8g2Fonts.getUTF8Width(buf);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(4 + hw + 2, 94);
                u8g2Fonts.print("%");
            }
        }

        // Wx ikona (desno, sc=8 — povečano za ~20% iz sc=7)
        if (weatherData.valid) {
            wxDrawConditionsScaled(92, 75, weatherData.wxCode, weatherData.isNight, 8, WX_ICON_LINESIZE);
        }

        // ══════════════════════════════════════════════════════════
        // CONA 2 — FanControl: temp/vlaga/napetost/tok + bara
        // y: 66–192
        // ══════════════════════════════════════════════════════════
        {
            bool shtErr = (sensorData.err & ERR_SHT30);
            bool inaErr = (sensorData.err & ERR_INA219);
            char buf[12];

            // ── 4-vrednostni blok: TEMP+VLAGA (vr.1) + NAPETOST+TOK (vr.2) ──

            // Labeli vrstica 1 — y=78
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4,  78); u8g2Fonts.print("TEMP");
            u8g2Fonts.setCursor(68, 78); u8g2Fonts.print("VLAGA");

            // Vrednosti vrstica 1 — y=91 (logisoso16)
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            if (shtErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.temp);
            u8g2Fonts.setCursor(4, 91); u8g2Fonts.print(buf);
            int16_t tw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4 + tw + 1, 91); u8g2Fonts.print("\xC2\xB0" "C");

            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            if (shtErr) snprintf(buf, sizeof(buf), "---");
            else        snprintf(buf, sizeof(buf), "%d", (int)roundf(sensorData.hum));
            u8g2Fonts.setCursor(68, 91); u8g2Fonts.print(buf);
            int16_t hw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(68 + hw + 1, 91); u8g2Fonts.print("%");

            // Labeli vrstica 2 — y=101
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4,  101); u8g2Fonts.print("NAPETOST");
            u8g2Fonts.setCursor(68, 101); u8g2Fonts.print("TOK");

            // Vrednosti vrstica 2 — y=114 (logisoso16)
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.volt);
            u8g2Fonts.setCursor(4, 114); u8g2Fonts.print(buf);
            int16_t vw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4 + vw + 1, 114); u8g2Fonts.print("V");

            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.2f", sensorData.amp);
            u8g2Fonts.setCursor(68, 114); u8g2Fonts.print(buf);
            int16_t aw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(68 + aw + 1, 114); u8g2Fonts.print("A");

            // ── VENTILATOR bar ────────────────────────────────────────────
            // label y=126, bar y=128 (w=84, h=14), val y=142, peak y=153
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 126); u8g2Fonts.print("VENTILATOR");

            {
                const int16_t bx = 4, by = 128, bw = 84, bh = 14;
                int16_t fill = (int16_t)((int32_t)bw * sensorData.fanPct / 100);
                drawFancyBar(bx, by, bw, bh, fill);

                char pctBuf[6];
                snprintf(pctBuf, sizeof(pctBuf), "%d%%", sensorData.fanPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(91, 142); u8g2Fonts.print(pctBuf);
            }

            // peak pod barom — y=153
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            {
                char pkBuf[10];
                snprintf(pkBuf, sizeof(pkBuf), "peak %d%%", peakFan);
                u8g2Fonts.setCursor(4, 153); u8g2Fonts.print(pkBuf);
            }

            // ── PORABA bar ────────────────────────────────────────────────
            // label y=165, bar y=167 (w=84, h=14), val y=181, peak y=192
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 165); u8g2Fonts.print("PORABA");

            {
                const int16_t bx = 4, by = 167, bw = 84, bh = 14;
                float peak = sensorData.peakWatt;
                if (peak < PEAK_WATT_MIN_FLOOR) peak = PEAK_WATT_DEFAULT;
                float ratio = inaErr ? 0.0f : (sensorData.watt / peak);
                if (ratio > 1.0f) ratio = 1.0f;
                int16_t fill = (int16_t)(bw * ratio);
                drawFancyBar(bx, by, bw, bh, fill);

                // vrednost desno — cela W + decimala (logisoso16)
                if (inaErr) {
                    u8g2Fonts.setFont(u8g2_font_profont12_mf);
                    u8g2Fonts.setCursor(91, 181); u8g2Fonts.print("--W");
                } else {
                    char wBuf[8];
                    snprintf(wBuf, sizeof(wBuf), "%dW", (int)roundf(sensorData.watt));
                    u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                    u8g2Fonts.setCursor(91, 181); u8g2Fonts.print(wBuf);
                }
            }

            // peak pod barom — y=192
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            {
                char pkBuf[12];
                float pk = sensorData.peakWatt;
                if (pk < PEAK_WATT_MIN_FLOOR) pk = PEAK_WATT_DEFAULT;
                snprintf(pkBuf, sizeof(pkBuf), "peak %dW", (int)roundf(pk));
                u8g2Fonts.setCursor(4, 192); u8g2Fonts.print(pkBuf);
            }
        }

        // ══════════════════════════════════════════════════════════
        // CONA 3 — NanoPi: promet, napadi, status
        // y: 196–296
        // ══════════════════════════════════════════════════════════
        {
            NanoPiData nd = nanopiGetData();

            // ── Status vrstica: SIRIUS + banIP ────────────────────────────
            // Ikona center y=203, r=7. Kljukica = OK, X = fail/unknown
            // SIRIUS (x center=11)
            {
                bool ok = nd.valid && nd.serverOk;
                display.fillCircle(11, 203, 7, GxEPD_BLACK);
                if (ok) {
                    // kljukica
                    display.drawLine(7, 203, 10, 207, GxEPD_WHITE);
                    display.drawLine(10, 207, 16, 197, GxEPD_WHITE);
                } else {
                    // X
                    display.drawLine(7, 198, 15, 208, GxEPD_WHITE);
                    display.drawLine(15, 198, 7, 208, GxEPD_WHITE);
                }
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(20, 207); u8g2Fonts.print("SIRIUS");
            }

            // banIP (x center=75)
            {
                bool ok = nd.valid && nd.banipRunning;
                display.fillCircle(75, 203, 7, GxEPD_BLACK);
                if (ok) {
                    display.drawLine(71, 203, 74, 207, GxEPD_WHITE);
                    display.drawLine(74, 207, 80, 197, GxEPD_WHITE);
                } else {
                    display.drawLine(71, 198, 79, 208, GxEPD_WHITE);
                    display.drawLine(79, 198, 71, 208, GxEPD_WHITE);
                }
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(84, 207); u8g2Fonts.print("banIP");
            }

            // ── PROMET bar ────────────────────────────────────────────────
            // label y=222, bar y=224 (w=84, h=14), 100% tick x=74
            // val% y=238 (logo16), sub y=253 (logo16 + profont12)
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 222); u8g2Fonts.print("PROMET");

            {
                const int16_t bx = 4, by = 224, bw = 84, bh = 14;
                // Skala 0–120%: 84px = 120%, fill clamped na bw
                uint8_t barPct  = nd.valid ? nd.networkBarPct : 0;
                int16_t fill    = (int16_t)min((int32_t)bw * barPct / 120, (int32_t)bw);
                drawFancyBar(bx, by, bw, bh, fill);

                // 100% tick = pikčasta navpična črta pri x=74
                // (100/120)*84 = 70px od levega roba → x = 4+70 = 74
                for (int16_t ty = by + 2; ty < by + bh - 2; ty += 2) {
                    display.drawPixel(74, ty, GxEPD_WHITE);  // pika v fill barvi
                }
                // Popravi barvo ticka glede na fill
                uint16_t tickCol = (fill > 70) ? GxEPD_WHITE : GxEPD_BLACK;
                for (int16_t ty = by + 2; ty < by + bh - 2; ty += 2) {
                    display.drawPixel(74, ty, tickCol);
                }

                // bar_pct vrednost desno (logo16) — y=238
                char pBuf[6];
                snprintf(pBuf, sizeof(pBuf), "%d%%", barPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(91, 238); u8g2Fonts.print(pBuf);
            }

            // Pod barom: logo16 za trenutno vrednost + profont12 za enoto+peak
            // Oba na isti baseline y=253
            {
                char curBuf[8], restBuf[24];
                if (!nd.valid) {
                    snprintf(curBuf,  sizeof(curBuf),  "--");
                    snprintf(restBuf, sizeof(restBuf), " Mb/s  peak --");
                } else {
                    snprintf(curBuf,  sizeof(curBuf),  "%.2f", nd.networkCurrentMbits);
                    snprintf(restBuf, sizeof(restBuf), " Mb/s  peak %.2f", nd.networkPeakMbits);
                }
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 253); u8g2Fonts.print(curBuf);
                int16_t xAfter = 4 + u8g2Fonts.getUTF8Width(curBuf);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(xAfter, 253); u8g2Fonts.print(restBuf);
            }

            // ── NAPADI bar ────────────────────────────────────────────────
            // label y=265, bar y=267 (w=84, h=14), 100% tick x=74
            // val% y=281 (logo16), sub y=296 (logo16 + profont12)
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 265); u8g2Fonts.print("NAPADI");

            {
                const int16_t bx = 4, by = 267, bw = 84, bh = 14;
                uint8_t barPct  = nd.valid ? nd.attacksBarPct : 0;
                int16_t fill    = (int16_t)min((int32_t)bw * barPct / 120, (int32_t)bw);
                drawFancyBar(bx, by, bw, bh, fill);

                // 100% tick (isti vzorec kot PROMET)
                uint16_t tickCol = (fill > 70) ? GxEPD_WHITE : GxEPD_BLACK;
                for (int16_t ty = by + 2; ty < by + bh - 2; ty += 2) {
                    display.drawPixel(74, ty, tickCol);
                }

                // bar_pct vrednost desno — y=281
                char pBuf[6];
                snprintf(pBuf, sizeof(pBuf), "%d%%", barPct);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(91, 281); u8g2Fonts.print(pBuf);
            }

            // Pod barom: logo16 + profont12 na baseline y=296
            {
                char curBuf[8], restBuf[24];
                if (!nd.valid) {
                    snprintf(curBuf,  sizeof(curBuf),  "--");
                    snprintf(restBuf, sizeof(restBuf), " /h  peak --");
                } else {
                    snprintf(curBuf,  sizeof(curBuf),  "%lu", (unsigned long)nd.attacksCurrentPh);
                    snprintf(restBuf, sizeof(restBuf), " /h  peak %lu", (unsigned long)nd.attacksPeakPh);
                }
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 296); u8g2Fonts.print(curBuf);
                int16_t xAfter = 4 + u8g2Fonts.getUTF8Width(curBuf);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(xAfter, 296); u8g2Fonts.print(restBuf);
            }

            // stale indikator — invertiran piksel v zgornjem desnem kotu Cone 3
            // (majhen vizualni namig brez dodatnega prostora)
            if (nd.stale && nd.valid) {
                display.fillRect(122, 196, 6, 6, GxEPD_BLACK);
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
