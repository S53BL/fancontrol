// display.cpp — GxEPD2 WeAct 2.9" B/W, 4-conski layout, pokončna orientacija
#include "display.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>

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

// Vrne slovensko kratico dneva
static const char* getDaySLO() {
    String d = myTZ.dateTime("D");
    if (d == "Mon") return "PON";
    if (d == "Tue") return "TOR";
    if (d == "Wed") return "SRE";
    if (d == "Thu") return "\xC4\x8C" "ET";  // Č + ET (UTF-8, potrebuje _mf font)
    if (d == "Fri") return "PET";
    if (d == "Sat") return "SOB";
    if (d == "Sun") return "NED";
    return "---";
}

bool initDisplay() {
    // 3a: GPIO pred SPI — preprečimo nedefiniran pin status (GxEPD2 crash)
    pinMode(PIN_EPD_DC,   OUTPUT);
    pinMode(PIN_EPD_RST,  OUTPUT);
    pinMode(PIN_EPD_CS,   OUTPUT);
    pinMode(PIN_EPD_BUSY, INPUT_PULLUP);

    SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
    delay(10);

    // 3b: BUSY timeout — floating pin ali zaslon ni priključen
    unsigned long t0 = millis();
    while (digitalRead(PIN_EPD_BUSY) == HIGH) {
        if (millis() - t0 >= EPD_BUSY_TIMEOUT_MS) {
            sensorData.err |= ERR_DISPLAY;
            LOG_WARN("EPD", "BUSY stuck HIGH po %lums — ePaper ni priključen", EPD_BUSY_TIMEOUT_MS);
            return false;
        }
        delay(10);
    }

    // 3c: Init samo če BUSY ni stuck
    display.init(115200, true, 2, false);
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
        // CONA 1 — Čas, DND, zunanja temperatura/vlaga, wx ikona
        // y: 0–116
        // ══════════════════════════════════════════════════════════════

        // --- Ura ---
        if (timeSynced) {
            String timeStr = myTZ.dateTime("H:i");
            u8g2Fonts.setFont(u8g2_font_logisoso24_tf);
            u8g2Fonts.setCursor(4, 34);
            u8g2Fonts.print(timeStr);

            // Dan (kratko)
            const char* day = getDaySLO();
            u8g2Fonts.setFont(u8g2_font_profont12_mf);
            u8g2Fonts.setCursor(4, 50);
            u8g2Fonts.print(day);
        } else {
            u8g2Fonts.setFont(u8g2_font_logisoso24_tf);
            u8g2Fonts.setCursor(4, 34);
            u8g2Fonts.print("--:--");
        }

        // --- DND luna ikona (desno zgoraj) ---
        if (sensorData.dndActive) {
            display.fillCircle(113, 14, 9, GxEPD_BLACK);
            display.fillCircle(117, 11, 8, GxEPD_WHITE);
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(105, 30);
            u8g2Fonts.print("DND");
        }

        // --- Zunanja temp + vlaga ---
        {
            char buf[12];
            u8g2Fonts.setFont(u8g2_font_profont10_mf);
            u8g2Fonts.setCursor(4, 63);
            u8g2Fonts.print("ZUNAJ");

            if (!weatherData.valid) {
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 84);
                u8g2Fonts.print("--.-");
                u8g2Fonts.setCursor(4, 104);
                u8g2Fonts.print("---%");
            } else {
                snprintf(buf, sizeof(buf), "%.1f", weatherData.outTemp);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 84);
                u8g2Fonts.print(buf);
                int16_t tw = u8g2Fonts.getUTF8Width(buf);
                u8g2Fonts.setFont(u8g2_font_profont10_mf);
                u8g2Fonts.setCursor(4 + tw + 2, 84);
                u8g2Fonts.print("\xC2\xB0" "C");

                snprintf(buf, sizeof(buf), "%d%%", (int)weatherData.outHum);
                u8g2Fonts.setFont(u8g2_font_logisoso16_tf);
                u8g2Fonts.setCursor(4, 108);
                u8g2Fonts.print(buf);
            }
        }

        // --- Vremenska ikona (desno, x=100, y=85) ---
        if (weatherData.valid) {
            wxDrawConditions(100, 85, weatherData.wxCode, weatherData.isNight);
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
