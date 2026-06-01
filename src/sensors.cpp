// sensors.cpp — Branje SHT30 in INA219 z graceful degradation
// Če senzor ni priključen ali ne odgovori → nastavi ERR flag, preskoči branje
// Koda se nikoli ne sesuje zaradi manjkajočega senzorja

#include "sensors.h"
#include "globals.h"
#include "logging.h"
#include <Adafruit_SHT31.h>
#include <Adafruit_INA219.h>
#include <Wire.h>
#include <Preferences.h>

static Adafruit_SHT31  sht30;
static Adafruit_INA219 ina219(ADDR_INA219);

// Stanje inicializacije — nastavljeno enkrat v initSensors()
static bool _sht30Ok  = false;
static bool _ina219Ok = false;

// --- Pomožna: I2C scan za en naslov ---
static bool i2cDevicePresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

// --- Inicializacija ---
bool initSensors() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    delay(50); // kratka stabilizacija busa

    // SHT30
    if (!i2cDevicePresent(ADDR_SHT30)) {
        LOG_WARN("SENS", "SHT30 ni najden na I2C 0x%02X", ADDR_SHT30);
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_SHT30;
        sensorData.temp = ERR_FLOAT;
        sensorData.hum  = ERR_FLOAT;
        portEXIT_CRITICAL(&dataMux);
        _sht30Ok = false;
    } else if (!sht30.begin(ADDR_SHT30)) {
        LOG_ERROR("SENS", "SHT30 begin() napaka");
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_SHT30;
        portEXIT_CRITICAL(&dataMux);
        _sht30Ok = false;
    } else {
        LOG_INFO("SENS", "SHT30 OK na 0x%02X", ADDR_SHT30);
        _sht30Ok = true;
    }

    // INA219
    if (!i2cDevicePresent(ADDR_INA219)) {
        LOG_WARN("SENS", "INA219 ni najden na I2C 0x%02X", ADDR_INA219);
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_INA219;
        sensorData.volt = ERR_FLOAT;
        sensorData.amp  = ERR_FLOAT;
        sensorData.watt = ERR_FLOAT;
        portEXIT_CRITICAL(&dataMux);
        _ina219Ok = false;
    } else if (!ina219.begin()) {
        LOG_ERROR("SENS", "INA219 begin() napaka");
        portENTER_CRITICAL(&dataMux);
        sensorData.err |= ERR_INA219;
        portEXIT_CRITICAL(&dataMux);
        _ina219Ok = false;
    } else {
        LOG_INFO("SENS", "INA219 OK na 0x%02X", ADDR_INA219);
        _ina219Ok = true;
    }

    return true; // vedno vrne true — napake so v ERR flagih
}

// --- Branje senzorjev ---
void readSensors() {

    // SHT30 — preskoči če ni inicializiran
    if (_sht30Ok) {
        float rawTemp = sht30.readTemperature();
        float rawHum  = sht30.readHumidity();

        if (isnan(rawTemp) || rawTemp < -20.0f || rawTemp > 80.0f) {
            LOG_WARN("SENS", "SHT30 temp izven obsega: %.1f", rawTemp);
            portENTER_CRITICAL(&dataMux);
            sensorData.err |= ERR_SHT30;
            portEXIT_CRITICAL(&dataMux);
        } else if (isnan(rawHum) || rawHum < 0.0f || rawHum > 100.0f) {
            LOG_WARN("SENS", "SHT30 vlaga izven obsega: %.1f", rawHum);
            portENTER_CRITICAL(&dataMux);
            sensorData.err |= ERR_SHT30;
            portEXIT_CRITICAL(&dataMux);
        } else {
            portENTER_CRITICAL(&dataMux);
            sensorData.temp = rawTemp + settings.tempOffset;
            sensorData.hum  = constrain(rawHum + settings.humOffset, 0.0f, 100.0f);
            sensorData.err &= ~ERR_SHT30;
            portEXIT_CRITICAL(&dataMux);
            LOG_INFO("SENS", "SHT30: T=%.1fdegC  H=%.1f%%  (offset T=%.1f H=%.1f)",
                     sensorData.temp, sensorData.hum,
                     settings.tempOffset, settings.humOffset);
        }
    }

    // INA219 — preskoči če ni inicializiran
    if (_ina219Ok) {
        float rawVolt = ina219.getBusVoltage_V();
        float rawAmp  = ina219.getCurrent_mA() / 1000.0f;

        if (rawVolt < 8.0f || rawVolt > 15.0f) {
            // Mini PC ni priključen ali izven obsega — ni napaka, normalno stanje
            portENTER_CRITICAL(&dataMux);
            sensorData.volt = rawVolt;   // vseeno zapiši za debug
            sensorData.amp  = 0.0f;
            sensorData.watt = 0.0f;
            sensorData.err &= ~ERR_INA219;
            portEXIT_CRITICAL(&dataMux);
            LOG_INFO("SENS", "INA219: V=%.2fV  A=0.00A  W=0.0W  (PC izklopljen, V izven 8-15V)", rawVolt);
        } else {
            float corrAmp  = rawAmp * settings.currentCorr;
            float calcWatt = rawVolt * corrAmp;
            portENTER_CRITICAL(&dataMux);
            sensorData.volt = rawVolt;
            sensorData.amp  = corrAmp;
            sensorData.watt = calcWatt;
            sensorData.err &= ~ERR_INA219;
            portEXIT_CRITICAL(&dataMux);
            updatePeakWatt();
            LOG_INFO("SENS", "INA219: V=%.2fV  A=%.3fA  W=%.2fW  (korekcija=%.2fx, peak=%.1fW)",
                     sensorData.volt, sensorData.amp, sensorData.watt,
                     settings.currentCorr, sensorData.peakWatt);
        }
    }

    portENTER_CRITICAL(&dataMux);
    newSensorData = true;
    portEXIT_CRITICAL(&dataMux);
}

bool sensorSht30Ok()  { return _sht30Ok; }
bool sensorIna219Ok() { return _ina219Ok; }

// Periodični retry za senzorje ki ob zagonu niso uspeli
void retrySensors() {
    if (!_sht30Ok) {
        LOG_INFO("SENS", "SHT30 retry inicializacije...");
        if (!i2cDevicePresent(ADDR_SHT30)) {
            LOG_WARN("SENS", "SHT30 retry: ni na I2C 0x%02X", ADDR_SHT30);
        } else if (!sht30.begin(ADDR_SHT30)) {
            LOG_WARN("SENS", "SHT30 retry: begin() napaka");
        } else {
            _sht30Ok = true;
            portENTER_CRITICAL(&dataMux);
            sensorData.err &= ~ERR_SHT30;
            portEXIT_CRITICAL(&dataMux);
            LOG_INFO("SENS", "SHT30 retry: OK");
        }
    }

    if (!_ina219Ok) {
        LOG_INFO("SENS", "INA219 retry inicializacije...");
        if (!i2cDevicePresent(ADDR_INA219)) {
            LOG_WARN("SENS", "INA219 retry: ni na I2C 0x%02X", ADDR_INA219);
        } else if (!ina219.begin()) {
            LOG_WARN("SENS", "INA219 retry: begin() napaka");
        } else {
            _ina219Ok = true;
            portENTER_CRITICAL(&dataMux);
            sensorData.err &= ~ERR_INA219;
            portEXIT_CRITICAL(&dataMux);
            LOG_INFO("SENS", "INA219 retry: OK");
        }
    }
}

// Peak watt avtokalibracija — kliče se po vsakem INA219 branju
void updatePeakWatt() {
    float current = sensorData.watt;

    if (current < PEAK_WATT_MIN_FLOOR) return;

    if (current > sensorData.peakWatt) {
        sensorData.peakWatt = current;

        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putFloat(PEAK_WATT_NVS_KEY, sensorData.peakWatt);
        prefs.end();

        LOG_INFO("SENS", "Nov peak watt: %.1f W (shranjeno v NVS)", sensorData.peakWatt);
    }
}
