# AGENT NAVODILO — Preflight check
**Projekt:** fancontrol (pametni ventilator za ACEMAGIC S1)
**Faza:** 00 — Preverjanje dostopnosti pred začetkom dela

---

## Tvoja naloga

Preveri dostopnost vseh potrebnih virov in potrdi pripravljenost za delo.
**Ne ustvarjaj še nobenih datotek. Ne pišeš še nobene kode.**
Samo preveri in poročaj.

---

## 1. Preveri referenčni projekt (samo branje)

Pot: `C:\PlatformIO\Projekti\vent_SEW`

Preveri da obstajajo naslednje datoteke:

- [ ] `platformio.ini`
- [ ] `src/main.cpp`
- [ ] `src/config.h`
- [ ] `src/globals.h`
- [ ] `src/globals.cpp`
- [ ] `src/graph_store.h`
- [ ] `src/graph_store.cpp`
- [ ] `src/sens.h`
- [ ] `src/sens.cpp`
- [ ] `src/web.h`
- [ ] `src/web.cpp`
- [ ] `src/web_handlers.h`
- [ ] `src/web_handlers.cpp`
- [ ] `src/http.h`
- [ ] `src/http.cpp`
- [ ] `partitions_8mb_ota.csv`

**POMEMBNO: V ta projekt ne pišeš ničesar.**

---

## 2. Preveri delovni projekt

Pot: `C:\PlatformIO\Projekti\fancontrol`

Preveri:
- [ ] Mapa `fancontrol` obstaja
- [ ] Podmapa `fancontrol\doc\` obstaja
- [ ] Datoteka `fancontrol\doc\ACEMAGIC_S1_FanController_IdejaV2.md` obstaja

Preberi `ACEMAGIC_S1_FanController_IdejaV2.md` in potrdi da si ga razumel — kratko povzemi (3-5 stavkov) kaj bo projekt delal.

---

## 3. Preveri PlatformIO okolje

Izvedi v terminalu:
```
pio --version
```

Potrdi da PlatformIO CLI deluje.

---

## 4. Poročilo

Ko si preveril vse zgornje točke, napiši kratko poročilo v obliki:

```
PREFLIGHT REPORT
================
vent_SEW referenčni projekt: OK / MANJKA [seznam]
fancontrol delovni projekt:  OK / MANJKA [seznam]
doc/IdejaV2.md:              OK / NI NAJDEN
PlatformIO CLI:              OK / NAPAKA
Povzetek idejnega projekta:  [3-5 stavkov]

STATUS: PRIPRAVLJEN ZA DELO / BLOKAN - [razlog]
```

---

## Kaj sledi (informativno — še ne delaj)

Po uspešnem preflight checku bova skupaj z razvijalcem napisala navodilo za Fazo 1: ustvarjanje skeleta projekta.
