// display.cpp — GxEPD2 WeAct 2.9" B/W, 4-conski layout, pokončna orientacija
#include "display.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "monitor.h"
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

    // Ticki na 20 / 40 / 60 / 80 %
    const uint8_t tickPcts[4] = {20, 40, 60, 80};
    for (uint8_t i = 0; i < 4; i++) {
        int16_t tx = bx + (int16_t)((int32_t)bw * tickPcts[i] / 100);
        uint16_t col = (tx < bx + fillW) ? GxEPD_WHITE : GxEPD_BLACK;
        display.drawFastVLine(tx, by + 2, bh - 4, col);
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
        display.drawFastHLine(4, 100, 120, GxEPD_BLACK);
        display.drawFastHLine(4, 185, 120, GxEPD_BLACK);
        display.drawFastHLine(4, 257, 120, GxEPD_BLACK);

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
        // CONA 2 — Notranja temp/vlaga, FAN bar
        // y: 103–185
        // ══════════════════════════════════════════════════════════

        {
            bool shtErr = (sensorData.err & ERR_SHT30);
            char buf[12];

            // Labeli
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 116);
            u8g2Fonts.print("TEMP");
            u8g2Fonts.setCursor(70, 116);
            u8g2Fonts.print("VLAGA");

            // Vrednosti — logisoso18
            if (shtErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.temp);
            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(4, 141);
            u8g2Fonts.print(buf);
            int16_t tw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4 + tw + 2, 141);
            u8g2Fonts.print("\xC2\xB0" "C");

            if (shtErr) snprintf(buf, sizeof(buf), "---");
            else        snprintf(buf, sizeof(buf), "%d", (int)roundf(sensorData.hum));
            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(70, 141);
            u8g2Fonts.print(buf);
            int16_t hw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(70 + hw + 2, 141);
            u8g2Fonts.print("%");

            // FAN bar (w=80, 10% ožji od prejšnjih 88)
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 157);
            u8g2Fonts.print("FAN");

            const int16_t fanBarX = 4, fanBarY = 161, fanBarW = 80, fanBarH = 12;
            int16_t fanFill = (int16_t)((int32_t)fanBarW * sensorData.fanPct / 100);
            drawFancyBar(fanBarX, fanBarY, fanBarW, fanBarH, fanFill);

            char pctBuf[6];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", sensorData.fanPct);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(fanBarX + fanBarW + 4, fanBarY + fanBarH);
            u8g2Fonts.print(pctBuf);
        }

        // ══════════════════════════════════════════════════════════
        // CONA 3 — Napetost, tok, moč bar
        // y: 188–253
        // ══════════════════════════════════════════════════════════

        {
            bool inaErr = (sensorData.err & ERR_INA219);
            char buf[10];

            // Labeli — NAPETOST cela beseda
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 201);
            u8g2Fonts.print("NAPETOST");
            u8g2Fonts.setCursor(68, 201);
            u8g2Fonts.print("TOK");

            // Vrednosti — logisoso18
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.volt);
            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(4, 223);
            u8g2Fonts.print(buf);
            int16_t vw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4 + vw + 2, 223);
            u8g2Fonts.print("V");

            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.2f", sensorData.amp);
            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(68, 223);
            u8g2Fonts.print(buf);
            int16_t aw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(68 + aw + 2, 223);
            u8g2Fonts.print("A");

            // MOC bar (w=80, enako kot FAN bar)
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 237);
            u8g2Fonts.print("MOC");

            const int16_t watBarX = 4, watBarY = 240, watBarW = 80, watBarH = 12;

            float peak = sensorData.peakWatt;
            if (peak < PEAK_WATT_MIN_FLOOR) peak = PEAK_WATT_DEFAULT;
            float ratio = inaErr ? 0.0f : (sensorData.watt / peak);
            if (ratio > 1.0f) ratio = 1.0f;
            int16_t watFill = (int16_t)(watBarW * ratio);
            drawFancyBar(watBarX, watBarY, watBarW, watBarH, watFill);

            {
                int16_t bx = watBarX + watBarW + 4;
                int16_t by = watBarY + watBarH;
                if (inaErr) {
                    u8g2Fonts.setFont(u8g2_font_profont12_mf);
                    u8g2Fonts.setCursor(bx, by);
                    u8g2Fonts.print("--W");
                } else {
                    char wholeBuf[8];
                    snprintf(wholeBuf, sizeof(wholeBuf), "%d.", (int)sensorData.watt);
                    u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                    u8g2Fonts.setCursor(bx, by);
                    u8g2Fonts.print(wholeBuf);
                    int16_t xAfter = bx + u8g2Fonts.getUTF8Width(wholeBuf);
                    char deciBuf[4];
                    snprintf(deciBuf, sizeof(deciBuf), "%dW",
                             (int)(sensorData.watt * 10.0f) % 10);
                    u8g2Fonts.setFont(u8g2_font_profont12_mf);
                    u8g2Fonts.setCursor(xAfter, by);
                    u8g2Fonts.print(deciBuf);
                }
            }
        }

        // ══════════════════════════════════════════════════════════
        // CONA 4 — PWR vtikač + NET grid
        // y: 261–294
        // ══════════════════════════════════════════════════════════

        {
            MonitorResult mon = monitorGetResult();
            bool wifiOk  = (WiFi.status() == WL_CONNECTED);
            bool inaErr  = (sensorData.err & ERR_INA219);

            // ── PWR vtikač ikona (x=4, y=262) ──────────────────
            {
                bool pwrOn = (!inaErr && mon.powered);

                // Pina: dva fillRect nad polkrogom
                display.fillRect(10, 262, 3, 5, GxEPD_BLACK);
                display.fillRect(17, 262, 3, 5, GxEPD_BLACK);

                // Polkrog (spodnja polovica kroga): center x=15, y=268, r=9
                // White rect pokriva y=259–266 → arc začne pri y=267 (baza)
                if (pwrOn) {
                    display.fillCircle(15, 268, 9, GxEPD_BLACK);
                    display.fillRect(6, 259, 19, 8, GxEPD_WHITE);
                    display.fillCircle(15, 268, 5, GxEPD_WHITE);
                } else {
                    display.drawCircle(15, 268, 9, GxEPD_BLACK);
                    display.fillRect(6, 259, 19, 8, GxEPD_WHITE);
                }

                // Pina znova (prekriti z belo zgoraj)
                display.fillRect(10, 262, 3, 5, GxEPD_BLACK);
                display.fillRect(17, 262, 3, 5, GxEPD_BLACK);

                // Vodoravna baza vtikača (poveže oba konca polkroga)
                display.drawFastHLine(6, 267, 18, GxEPD_BLACK);

                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(4, 291);
                u8g2Fonts.print(inaErr ? "POWER ?" : (pwrOn ? "POWER ON" : "POWER OFF"));
            }

            // ── NET grid 3×3 (x=68, y=262) ─────────────────────
            // Vsaka celica: 5×5px, razmak 1px → korak 6px
            {
                PortEntry* ports = monitorGetPorts();
                int totalPorts = 0;
                int okPorts    = 0;
                bool anyFail   = false;

                for (int i = 0; i < MONITOR_MAX_PORTS; i++) {
                    if (ports[i].enabled && ports[i].port > 0) {
                        totalPorts++;
                        if (ports[i].lastOk) okPorts++;
                        else anyFail = true;
                    }
                }

                const int16_t gx   = 68;
                const int16_t gy   = 262;
                const int16_t cs   = 5;
                const int16_t gap  = 1;
                const int16_t step = cs + gap;

                int portIdx = 0;
                for (int row = 0; row < 3; row++) {
                    for (int col = 0; col < 3; col++) {
                        int16_t cx = gx + col * step;
                        int16_t cy = gy + row * step;
                        if (portIdx < totalPorts) {
                            if (ports[portIdx].lastOk)
                                display.fillRect(cx, cy, cs, cs, GxEPD_BLACK);
                            else
                                display.drawRect(cx, cy, cs, cs, GxEPD_BLACK);
                        } else {
                            display.drawPixel(cx + 2, cy + 2, GxEPD_BLACK);
                        }
                        portIdx++;
                    }
                }

                char netBuf[16];
                if (!wifiOk || totalPorts == 0)
                    snprintf(netBuf, sizeof(netBuf), "NET  --");
                else if (!anyFail)
                    snprintf(netBuf, sizeof(netBuf), "NET  %d/%d", okPorts, totalPorts);
                else
                    snprintf(netBuf, sizeof(netBuf), "ERR  %d/%d", okPorts, totalPorts);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(68, 291);
                u8g2Fonts.print(netBuf);
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
