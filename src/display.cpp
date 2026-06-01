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
#define FONT_LABEL  u8g2_font_profont10_mf
#define FONT_VALUE  u8g2_font_logisoso16_tf
#define FONT_TIME   u8g2_font_logisoso28_tf
#define FONT_DAY    u8g2_font_profont17_mf
#define FONT_UNIT   u8g2_font_profont10_mf

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

    LOG_INFO("EPD", "clearScreen OK");

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
        u8g2Fonts.setFont(u8g2_font_profont10_mf);

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

void updateDisplay() {
    if (sensorData.err & ERR_DISPLAY) {
        LOG_WARN("EPD", "updateDisplay() preskočen — ERR_DISPLAY");
        return;
    }

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // ── Ločilne črte ──────────────────────────────────────────────
        display.drawFastHLine(4, 117, 120, GxEPD_BLACK);
        display.drawFastHLine(4, 211, 120, GxEPD_BLACK);

        // ══════════════════════════════════════════════════════════════
        // CONA 1 — Čas, datum, sunrise/sunset, zunanja temp/vlaga, wx ikona
        // y: 0–116
        // ══════════════════════════════════════════════════════════════

        // --- Ura (levo zgoraj, velik font) ---
        if (timeSynced) {
            String timeStr = myTZ.dateTime("H:i");
            u8g2Fonts.setFont(u8g2_font_logisoso24_tf);
            u8g2Fonts.setCursor(4, 34);
            u8g2Fonts.print(timeStr);
        } else {
            u8g2Fonts.setFont(u8g2_font_logisoso24_tf);
            u8g2Fonts.setCursor(4, 34);
            u8g2Fonts.print("--:--");
        }

        // --- Sunrise / Sunset desno od ure v dveh vrsticah ---
        // Pozicija: x=70 (desna polovica), vrstici y=20 in y=34
        {
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            char buf[12];

            // Vrstica 1: vzh HH:MM
            snprintf(buf, sizeof(buf), "vzh %s", weatherData.sunrise);
            u8g2Fonts.setCursor(70, 22);
            u8g2Fonts.print(buf);

            // Vrstica 2: zah HH:MM
            snprintf(buf, sizeof(buf), "zah %s", weatherData.sunset);
            u8g2Fonts.setCursor(70, 34);
            u8g2Fonts.print(buf);
        }

        // --- Datum: cela beseda + datum (levo, pod uro) ---
        if (timeSynced) {
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            char dateBuf[32];
            // Format: "Ponedeljek 1.6.2026"
            snprintf(dateBuf, sizeof(dateBuf), "%s %d.%d.%d",
                     getDaySLO(),
                     myTZ.day(), myTZ.month(), myTZ.year());
            u8g2Fonts.setCursor(4, 46);
            u8g2Fonts.print(dateBuf);
        }

        // --- DND luna ikona (desno zgoraj, samo če aktiven) ---
        if (sensorData.dndActive) {
            display.fillCircle(113, 8, 7, GxEPD_BLACK);
            display.fillCircle(116, 5, 6, GxEPD_WHITE);
        }

        // --- Ločilna črta med uro/datumom in zunanjo cono ---
        display.drawFastHLine(4, 50, 120, GxEPD_BLACK);

        // --- Zunanja temp + vlaga (levo, pod ločilno črto) ---
        {
            char buf[12];
            if (!weatherData.valid) {
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 72);
                u8g2Fonts.print("--.-");
                u8g2Fonts.setFont(u8g2_font_profont10_mf);
                u8g2Fonts.setCursor(4, 84);
                u8g2Fonts.print("---%");
            } else {
                // Temperatura
                snprintf(buf, sizeof(buf), "%.1f", weatherData.outTemp);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 72);
                u8g2Fonts.print(buf);
                int16_t tw = u8g2Fonts.getUTF8Width(buf);
                u8g2Fonts.setFont(u8g2_font_profont10_mf);
                u8g2Fonts.setCursor(4 + tw + 2, 72);
                u8g2Fonts.print("\xC2\xB0" "C");

                // Vlaga
                snprintf(buf, sizeof(buf), "%d%%", (int)weatherData.outHum);
                u8g2Fonts.setFont(u8g2_font_profont12_mf);
                u8g2Fonts.setCursor(4, 85);
                u8g2Fonts.print(buf);
            }
        }

        // --- Vremenska ikona (desno, maximalna velikost v razpoložljivem prostoru) ---
        // Razpoložljiv prostor desno: x od ~60 do 124, y od 52 do 114 → ~64×62px
        // Center ikone: x=92, y=83
        if (weatherData.valid) {
            const int wxScale = 7;
            const int wxLS    = WX_ICON_LINESIZE;
            wxDrawConditionsScaled(92, 83, weatherData.wxCode, weatherData.isNight, wxScale, wxLS);
        }

        // ══════════════════════════════════════════════════════════════
        // CONA 2 — Notranja temp/vlaga, fan bar
        // y: 120–210
        // ══════════════════════════════════════════════════════════════

        {
            bool shtErr = (sensorData.err & ERR_SHT30);
            char buf[12];

            // Notranja temp (levo)
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4, 133);
            u8g2Fonts.print("TEMP");

            if (shtErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.temp);

            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(4, 158);
            u8g2Fonts.print(buf);
            int16_t tw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4 + tw + 2, 158);
            u8g2Fonts.print("\xC2\xB0" "C");

            // Notranja vlaga (desno)
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(70, 133);
            u8g2Fonts.print("VLAGA");

            if (shtErr) snprintf(buf, sizeof(buf), "---");
            else        snprintf(buf, sizeof(buf), "%d%%", (int)roundf(sensorData.hum));

            u8g2Fonts.setFont(u8g2_font_logisoso18_tf);
            u8g2Fonts.setCursor(70, 158);
            u8g2Fonts.print(buf);

            // Fan bar
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4, 174);
            u8g2Fonts.print("FAN");

            const int16_t fanBarX = 4, fanBarY = 178, fanBarW = 88, fanBarH = 12;
            display.drawRect(fanBarX, fanBarY, fanBarW, fanBarH, GxEPD_BLACK);
            int16_t fanFill = (int16_t)((int32_t)fanBarW * sensorData.fanPct / 100);
            if (fanFill > 0) display.fillRect(fanBarX, fanBarY, fanFill, fanBarH, GxEPD_BLACK);

            char pctBuf[6];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", sensorData.fanPct);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(fanBarX + fanBarW + 4, fanBarY + fanBarH);
            u8g2Fonts.print(pctBuf);
        }

        // ══════════════════════════════════════════════════════════════
        // CONA 3 — Napetost, tok, moč bar
        // y: 214–296
        // ══════════════════════════════════════════════════════════════

        {
            bool inaErr = (sensorData.err & ERR_INA219);
            char buf[10];

            // Napetost (levo)
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4, 227);
            u8g2Fonts.print("NAPET.");

            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.volt);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(4, 248);
            u8g2Fonts.print(buf);
            int16_t vw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4 + vw + 2, 248);
            u8g2Fonts.print("V");

            // Tok (desno)
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(68, 227);
            u8g2Fonts.print("TOK");

            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.2f", sensorData.amp);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(68, 248);
            u8g2Fonts.print(buf);
            int16_t aw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(68 + aw + 2, 248);
            u8g2Fonts.print("A");

            // Moč bar (avtokalibriran na peakWatt)
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4, 263);
            u8g2Fonts.print("MO\xC4\x8C");  // MOČ v UTF-8

            const int16_t watBarX = 4, watBarY = 267, watBarW = 88, watBarH = 12;
            display.drawRect(watBarX, watBarY, watBarW, watBarH, GxEPD_BLACK);

            float peak = sensorData.peakWatt;
            if (peak < PEAK_WATT_MIN_FLOOR) peak = PEAK_WATT_DEFAULT;
            float ratio = inaErr ? 0.0f : (sensorData.watt / peak);
            if (ratio > 1.0f) ratio = 1.0f;
            int16_t watFill = (int16_t)(watBarW * ratio);
            if (watFill > 0) display.fillRect(watBarX, watBarY, watFill, watBarH, GxEPD_BLACK);

            if (inaErr) snprintf(buf, sizeof(buf), "--W");
            else        snprintf(buf, sizeof(buf), "%.1fW", sensorData.watt);
            u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
            u8g2Fonts.setCursor(watBarX + watBarW + 4, watBarY + watBarH);
            u8g2Fonts.print(buf);

            // --- Monitor ikoni (spodnji levi del cone 3) ---
            {
                MonitorResult mon = monitorGetResult();
                bool wifiOk = (WiFi.status() == WL_CONNECTED);
                u8g2Fonts.setFont(u8g2_font_profont10_mf);

                // PWR ikona
                const char* pwrStr;
                if (inaErr)        pwrStr = "PWR ?";
                else if (mon.powered) pwrStr = "PWR ON";
                else               pwrStr = "PWR OF";
                u8g2Fonts.setCursor(4, 292);
                u8g2Fonts.print(pwrStr);

                // NET ikona
                const char* netStr;
                if (!wifiOk)           netStr = "NET --";
                else if (mon.allPortsOk)  netStr = "NET OK";
                else if (mon.anyPortFail) netStr = "NET ER";
                else                   netStr = "NET --";
                u8g2Fonts.setCursor(64, 292);
                u8g2Fonts.print(netStr);
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
