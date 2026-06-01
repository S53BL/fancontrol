// suntime.h — Lokalni izračun časa sončnega vzhoda in zahoda
// Algoritem: Duffett-Smith / NOAA formula
// Natančnost: ±1–2 minuti (zadostuje za prikaz HH:MM)
#pragma once

// Izračuna sunrise in sunset za dani datum in koordinate.
// Izhod: sunrise_out in sunset_out kot "HH:MM" stringi (6 znakov z null).
// Če izračun ne uspe (polarna noč/dan), nastavi "--:--".
// utcOffsetHours: odmik od UTC v urah (npr. 1 za CET, 2 za CEST)
void calcSunTimes(int day, int month, int year,
                  float lat, float lon,
                  int utcOffsetHours,
                  char* sunrise_out, char* sunset_out);

// Pomožna: vrne UTC offset glede na datum (1=CET, 2=CEST) za Slovenijo
// Upošteva prehod na poletni čas (zadnja nedelja marca / zadnja nedelja oktobra)
int getCETOffset(int day, int month, int year);
