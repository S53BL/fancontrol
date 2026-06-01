// led.cpp — Implementacija RGB LED
// Uporablja Adafruit NeoPixel knjižnjico (1 LED, pin iz config.h PIN_RGB_LED)
// NeoPixel je WS2812B kompatibilen — 3-žilni protokol

#include "led.h"
#include "config.h"
#include "globals.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel _led(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

void ledInit() {
    _led.begin();
    _led.setBrightness(RGB_BRIGHTNESS);
    _led.clear();
    _led.show();
}

void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    if (!settings.ledEnabled) {
        _led.setPixelColor(0, _led.Color(0, 0, 0));
        _led.show();
        return;
    }
    _led.setPixelColor(0, _led.Color(r, g, b));
    _led.show();
}

void ledBlue()   { ledSet(0,   0,   255); }
void ledYellow() { ledSet(255, 180, 0);   }
void ledGreen()  { ledSet(0,   255, 0);   }
void ledRed()    { ledSet(255, 0,   0);   }
void ledOrange() { ledSet(255, 80,  0);   }
void ledOff()    { ledSet(0,   0,   0);   }
