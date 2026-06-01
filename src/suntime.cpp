// suntime.cpp — Lokalni izračun časa sončnega vzhoda in zahoda
// Algoritem: Duffett-Smith (prilagojen), identičen NOAA Solar Calculator
#include "suntime.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float degToRad(float d) { return d * (float)M_PI / 180.0f; }
static float radToDeg(float r) { return r * 180.0f / (float)M_PI; }

// Julian Day Number iz datuma
static long julianDay(int day, int month, int year) {
    if (month <= 2) { year--; month += 12; }
    long A = year / 100;
    long B = 2 - A + A / 4;
    return (long)(365.25f * (year + 4716)) + (long)(30.6001f * (month + 1)) + day + B - 1524;
}

// UTC offset za Slovenijo (CET=+1, CEST=+2)
// Prehod: zadnja nedelja marca ob 2:00 → CEST; zadnja nedelja oktobra ob 3:00 → CET
int getCETOffset(int day, int month, int year) {
    if (month < 3 || month > 10) return 1;
    if (month > 3 && month < 10) return 2;

    // Zadnja nedelja v mesecu: najdi dan tedna za 31. (ali 30.) in izračunaj
    // Weekday po Sakamoto: 0=ned, 1=pon, ...
    auto wday = [](int d, int m, int y) -> int {
        static int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
        if (m < 3) y--;
        return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    };

    int lastDay = (month == 3) ? 31 : 31; // oba meseca imata 31 dni
    int wd = wday(lastDay, month, year);
    int lastSun = lastDay - wd; // dan zadnje nedelje

    if (month == 3) return (day >= lastSun) ? 2 : 1;
    else            return (day >= lastSun) ? 1 : 2;
}

void calcSunTimes(int day, int month, int year,
                  float lat, float lon,
                  int utcOffsetHours,
                  char* sunrise_out, char* sunset_out) {

    // Varnostni fallback
    snprintf(sunrise_out, 6, "--:--");
    snprintf(sunset_out,  6, "--:--");

    long jd = julianDay(day, month, year);
    float d = (float)(jd - 2451545L); // dni od J2000.0

    // Srednja dolžina sonca [°]
    float L = fmodf(280.460f + 0.9856474f * d, 360.0f);
    if (L < 0) L += 360.0f;

    // Srednja anomalija [°]
    float g = fmodf(357.528f + 0.9856003f * d, 360.0f);
    if (g < 0) g += 360.0f;

    // Ekliptična dolžina [°]
    float lambda = L + 1.915f * sinf(degToRad(g)) + 0.020f * sinf(degToRad(2.0f * g));

    // Nagib ekliptike [°]
    float epsilon = 23.439f - 0.0000004f * d;

    // Deklinacija [rad]
    float sinDec = sinf(degToRad(epsilon)) * sinf(degToRad(lambda));
    float dec = asinf(sinDec);

    // Enačba časa [min] — aproksimacija
    float RA = radToDeg(atan2f(cosf(degToRad(epsilon)) * sinf(degToRad(lambda)),
                                cosf(degToRad(lambda))));
    RA = fmodf(RA, 360.0f);
    if (RA < 0) RA += 360.0f;
    float EqT = (L - RA);
    if (EqT >  180.0f) EqT -= 360.0f;
    if (EqT < -180.0f) EqT += 360.0f;
    EqT *= 4.0f; // stopinje → minute

    // Urni kot za sunrise/sunset (refraction + solar disk korekcija: -0.8333°)
    float cosH = (sinf(degToRad(-0.8333f)) - sinf(degToRad(lat)) * sinDec)
                 / (cosf(degToRad(lat)) * cosf(dec));

    // Polarni dan ali noč
    if (cosH < -1.0f) {
        // Poldan — sonce ne zahaja
        snprintf(sunrise_out, 6, "00:00");
        snprintf(sunset_out,  6, "23:59");
        return;
    }
    if (cosH > 1.0f) {
        // Polarna noč — sonce ne vzide
        snprintf(sunrise_out, 6, "--:--");
        snprintf(sunset_out,  6, "--:--");
        return;
    }

    float H = radToDeg(acosf(cosH)); // urni kot [°]

    // Solarni opoldne (transit) — UTC minute
    float transit = 720.0f - 4.0f * lon - EqT;

    // Sunrise in sunset — UTC minute od polnoči
    float sunriseUTC = transit - H * 4.0f;
    float sunsetUTC  = transit + H * 4.0f;

    // Pretvorba v lokalni čas
    float sunriseLoc = sunriseUTC + utcOffsetHours * 60.0f;
    float sunsetLoc  = sunsetUTC  + utcOffsetHours * 60.0f;

    // Normalizacija (0–1440 min/dan)
    sunriseLoc = fmodf(sunriseLoc + 1440.0f, 1440.0f);
    sunsetLoc  = fmodf(sunsetLoc  + 1440.0f, 1440.0f);

    int srH = (int)(sunriseLoc / 60.0f);
    int srM = (int)fmodf(sunriseLoc, 60.0f);
    int ssH = (int)(sunsetLoc  / 60.0f);
    int ssM = (int)fmodf(sunsetLoc,  60.0f);

    snprintf(sunrise_out, 6, "%02d:%02d", srH, srM);
    snprintf(sunset_out,  6, "%02d:%02d", ssH, ssM);
}
