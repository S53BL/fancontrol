// display.cpp — GxEPD2 WeAct 2.9" B/W, 4-conski layout, pokončna orientacija
#include "display.h"
#include "config.h"
#include "globals.h"
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

void initDisplay() {
    SPI.begin(PIN_EPD_CLK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
    display.init(115200, true, 2, false);
    display.setRotation(0);         // pokončna orientacija
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
    u8g2Fonts.begin(display);
    u8g2Fonts.setFontMode(1);              // transparentno ozadje
    u8g2Fonts.setFontDirection(0);         // levo → desno
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
}

void updateDisplay() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // --- Ločilne črte med conami ---
        display.drawFastHLine(4, 52,  120, GxEPD_BLACK);
        display.drawFastHLine(4, 128, 120, GxEPD_BLACK);
        display.drawFastHLine(4, 200, 120, GxEPD_BLACK);

        // =============================================
        // CONA 1 — Čas in dan (y: 0–52)
        // =============================================
        if (timeSynced) {
            // Ura — centrirana
            String timeStr = myTZ.dateTime("H:i");
            u8g2Fonts.setFont(FONT_TIME);
            u8g2Fonts.setCursor(centerX(timeStr.c_str(), 0), 40);
            u8g2Fonts.print(timeStr);

            // Dan — desno poravnan
            const char* day = getDaySLO();
            u8g2Fonts.setFont(FONT_DAY);
            int16_t dayW = u8g2Fonts.getUTF8Width(day);
            u8g2Fonts.setCursor(EPD_WIDTH - dayW - 4, 40);
            u8g2Fonts.print(day);
        } else {
            u8g2Fonts.setFont(FONT_TIME);
            u8g2Fonts.setCursor(centerX("--:--", 0), 40);
            u8g2Fonts.print("--:--");
        }

        // =============================================
        // CONA 2 — Senzorji (y: 52–128)
        // =============================================
        {
            bool shtErr = (sensorData.err & ERR_SHT30);
            char buf[12];

            // --- Temperatura ---
            u8g2Fonts.setFont(FONT_LABEL);
            u8g2Fonts.setCursor(6, 70);
            u8g2Fonts.print("TEMP");

            if (shtErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.temp);

            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(42, 90);
            u8g2Fonts.print(buf);
            int16_t tw = u8g2Fonts.getUTF8Width(buf);

            u8g2Fonts.setFont(FONT_UNIT);
            u8g2Fonts.setCursor(42 + tw + 2, 90);
            u8g2Fonts.print("\xC2\xB0" "C");  // °C v UTF-8 (potrebuje _mf font)

            // --- Vlažnost ---
            u8g2Fonts.setFont(FONT_LABEL);
            u8g2Fonts.setCursor(6, 100);
            u8g2Fonts.print("VLAGA");

            if (shtErr) snprintf(buf, sizeof(buf), "---");
            else        snprintf(buf, sizeof(buf), "%d", (int)roundf(sensorData.hum));

            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(42, 120);
            u8g2Fonts.print(buf);
            int16_t hw = u8g2Fonts.getUTF8Width(buf);

            u8g2Fonts.setFont(FONT_UNIT);
            u8g2Fonts.setCursor(42 + hw + 2, 120);
            u8g2Fonts.print("%");
        }

        // =============================================
        // CONA 3 — INA219 poraba Mini PC (y: 128–200)
        // =============================================
        {
            bool inaErr = (sensorData.err & ERR_INA219);
            char buf[10];

            // Napetost
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.volt);
            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(4, 148);
            u8g2Fonts.print(buf);
            int16_t vw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(FONT_UNIT);
            u8g2Fonts.setCursor(4 + vw + 2, 148);
            u8g2Fonts.print("V");

            // Tok
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.2f", sensorData.amp);
            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(68, 148);
            u8g2Fonts.print(buf);
            int16_t aw = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(FONT_UNIT);
            u8g2Fonts.setCursor(68 + aw + 2, 148);
            u8g2Fonts.print("A");

            // Moč
            if (inaErr) snprintf(buf, sizeof(buf), "--.-");
            else        snprintf(buf, sizeof(buf), "%.1f", sensorData.watt);
            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(4, 178);
            u8g2Fonts.print(buf);
            int16_t ww = u8g2Fonts.getUTF8Width(buf);
            u8g2Fonts.setFont(FONT_UNIT);
            u8g2Fonts.setCursor(4 + ww + 2, 178);
            u8g2Fonts.print("W");
        }

        // =============================================
        // CONA 4 — Ventilator (y: 200–296)
        // =============================================
        {
            u8g2Fonts.setFont(FONT_LABEL);
            u8g2Fonts.setCursor(4, 218);
            u8g2Fonts.print("FAN");

            // Progress bar: okvir + zapolnjen del proporcionalno fanPct
            const int16_t barX = 4, barY = 222, barW = 80, barH = 12;
            display.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
            int16_t fillW = (int16_t)((int32_t)barW * sensorData.fanPct / 100);
            if (fillW > 0) display.fillRect(barX, barY, fillW, barH, GxEPD_BLACK);

            // Odstotek desno od bara
            char pctBuf[8];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", sensorData.fanPct);
            u8g2Fonts.setFont(FONT_VALUE);
            u8g2Fonts.setCursor(barX + barW + 4, barY + barH);
            u8g2Fonts.print(pctBuf);

            // DND status
            u8g2Fonts.setFont(FONT_LABEL);
            u8g2Fonts.setCursor(4, 250);
            u8g2Fonts.print(sensorData.dndActive ? "DND: ON" : "DND: OFF");
        }

    } while (display.nextPage());
}
