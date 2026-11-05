#pragma once
#include <stdint.h>
#include <ArduinoJson.h>

namespace ldg {
struct BrightnessSettings {
    uint8_t automatic = 0, manual = 100, minimum = 15, maximum = 100;
    uint16_t darkAdc = 3500, brightAdc = 300;
};
inline bool validBrightnessSettings(const BrightnessSettings& s) {
    const int span = int(s.darkAdc) - int(s.brightAdc);
    return s.automatic <= 1 && s.manual >= 1 && s.manual <= 100 && s.minimum >= 1 &&
        s.maximum <= 100 && s.minimum <= s.maximum && s.darkAdc <= 4095 && s.brightAdc <= 4095 &&
        (span >= 100 || span <= -100);
}
struct StoredBrightnessSettings { uint32_t version = 1; BrightnessSettings value; };
template<class Storage> bool writeBrightnessSettings(Storage& storage, const BrightnessSettings& value) {
    if (!validBrightnessSettings(value)) return false;
    StoredBrightnessSettings stored;
    stored.value = value;
    return storage.putBytes("backlightV1", &stored, sizeof(stored)) == sizeof(stored);
}
template<class Storage> bool readBrightnessSettings(Storage& storage, BrightnessSettings& value) {
    StoredBrightnessSettings stored;
    if (storage.getBytesLength("backlightV1") != sizeof(stored) ||
        storage.getBytes("backlightV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || !validBrightnessSettings(stored.value)) return false;
    value = stored.value;
    return true;
}
inline void writeBrightnessJson(JsonDocument& doc, const BrightnessSettings& s) {
    doc["automatic"] = bool(s.automatic); doc["manual"] = s.manual;
    doc["minimum"] = s.minimum; doc["maximum"] = s.maximum;
    doc["dark_adc"] = s.darkAdc; doc["bright_adc"] = s.brightAdc;
}
inline bool parseBrightnessSettings(JsonVariantConst doc, BrightnessSettings& out) {
    if (!doc.is<JsonObjectConst>() || !doc["automatic"].is<bool>() ||
        !doc["manual"].is<uint8_t>() || !doc["minimum"].is<uint8_t>() || !doc["maximum"].is<uint8_t>() ||
        !doc["dark_adc"].is<uint16_t>() || !doc["bright_adc"].is<uint16_t>()) return false;
    BrightnessSettings s;
    s.automatic = doc["automatic"].as<bool>(); s.manual = doc["manual"].as<uint8_t>();
    s.minimum = doc["minimum"].as<uint8_t>(); s.maximum = doc["maximum"].as<uint8_t>();
    s.darkAdc = doc["dark_adc"].as<uint16_t>(); s.brightAdc = doc["bright_adc"].as<uint16_t>();
    if (!validBrightnessSettings(s)) return false;
    out = s;
    return true;
}
// Call at 100 ms intervals. EMA rejects short flashes/noise; a 2% deadband
// and a bounded 2%-per-sample fade avoid visible brightness oscillation.
class BrightnessControl {
public:
    uint8_t update(const BrightnessSettings& s, int raw) {
        if (!validBrightnessSettings(s)) return m_percent = 100;
        if (raw < 0 || raw > 4095 || !s.automatic) {
            m_filtered = -1;
            return m_percent = s.manual;
        }
        m_filtered = m_filtered < 0 ? raw : (3 * m_filtered + raw + 2) / 4;
        const int span = int(s.brightAdc) - int(s.darkAdc);
        int light = (m_filtered - int(s.darkAdc)) * 1000 / span;
        if (light < 0) light = 0;
        if (light > 1000) light = 1000;
        const int target = s.minimum + ((s.maximum - s.minimum) * light + 500) / 1000;
        const int delta = target - m_percent;
        if (delta > 2) m_percent += 2;
        else if (delta < -2) m_percent -= 2;
        else if (target == s.minimum || target == s.maximum) m_percent = target;
        if (m_percent < s.minimum) m_percent = s.minimum;
        if (m_percent > s.maximum) m_percent = s.maximum;
        return m_percent;
    }
private:
    int m_filtered = -1;
    uint8_t m_percent = 100;
};
}
