#include "display_backlight.h"
#include "config_manager.h"
#include <Arduino.h>

DisplayBacklight displayBacklight;
void DisplayBacklight::begin() {
#ifdef BOARD_ESP32_3248S035C
    pinMode(34, INPUT);
    analogReadResolution(12);
    // The board BSP uses 0 dB for its light-sensor voltage range. Configure
    // this pin only, leaving any other ADC inputs untouched.
    analogSetPinAttenuation(34, ADC_0db);
    m_ready = ledcAttachChannel(27, 20000, 8, 7);
    if (m_ready) {
        ledcWriteChannel(7, 255);
        m_duty = 255;
        m_lastRead = millis() - 100;
        loop();
    } else {
        digitalWrite(27, HIGH);
        Serial.println("ERROR: backlight PWM unavailable; using full brightness");
    }
#endif
}
void DisplayBacklight::loop() {
#ifdef BOARD_ESP32_3248S035C
    if (!m_ready || millis() - m_lastRead < 100) return;
    m_lastRead = millis();
    // Four bounded ADC reads per 100 ms; no sleeps, NVS writes or LVGL calls.
    int raw = 0;
    for (int i = 0; i < 4; ++i) raw += analogRead(34);
    raw = (raw + 2) / 4;
    const uint8_t percent = m_control.update(configManager.brightnessSettings(), raw);
    const uint8_t duty = (unsigned(percent) * 255 + 50) / 100;
    if (duty != m_duty) { ledcWriteChannel(7, duty); m_duty = duty; }
    m_sample.store(0x80000000u | (uint32_t(percent) << 12) | uint32_t(raw));
#endif
}
void DisplayBacklight::writeStatus(JsonObject doc) const {
    doc["supported"] = supported();
    const uint32_t sample = m_sample.load();
    doc["active"] = bool(sample & 0x80000000u);
    if (sample & 0x80000000u) {
        doc["light_raw"] = sample & 4095u;
        doc["brightness"] = (sample >> 12) & 127u;
    } else { doc["light_raw"] = nullptr; doc["brightness"] = nullptr; }
}
