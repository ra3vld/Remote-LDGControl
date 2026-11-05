#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include "meter_mapping.h"

namespace ldg {
struct MeterSettings {
    uint16_t watts = 1000;
    bool logarithmic = false;
};
inline bool validMeterSettings(const MeterSettings& settings) {
    return settings.watts >= 1 && settings.watts <= 1000;
}
struct StoredMeterSettings {
    uint32_t version = 1;
    uint16_t watts = 1000;
    uint8_t logarithmic = 0;
    uint8_t reserved = 0;
};
template<class Storage> bool writeMeterSettings(Storage& storage, const MeterSettings& settings) {
    if (!validMeterSettings(settings)) return false;
    StoredMeterSettings stored;
    stored.watts = settings.watts;
    stored.logarithmic = settings.logarithmic;
    return storage.putBytes("meterScaleV1", &stored, sizeof(stored)) == sizeof(stored);
}
template<class Storage> bool readMeterSettings(Storage& storage, MeterSettings& settings) {
    StoredMeterSettings stored;
    if (storage.getBytesLength("meterScaleV1") != sizeof(stored) ||
        storage.getBytes("meterScaleV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || stored.logarithmic > 1 || stored.reserved ||
        stored.watts < 1 || stored.watts > 1000) return false;
    settings.watts = stored.watts;
    settings.logarithmic = stored.logarithmic;
    return true;
}

// A short critical section keeps value, mapping and full 32-bit revision
// coherent across the web task and UI task. No I/O occurs inside it.
class MeterScale {
public:
    struct Snapshot { uint16_t watts; uint32_t revision; bool logarithmic; };
    Snapshot snapshot() const {
        portENTER_CRITICAL(&m_mux);
        const Snapshot value{m_settings.watts, m_revision, m_settings.logarithmic};
        portEXIT_CRITICAL(&m_mux);
        return value;
    }
    bool set(uint16_t watts) { return update(watts, false, false); }
    bool set(uint16_t watts, bool logarithmic) { return update(watts, logarithmic, true); }
    void setLogarithmic(bool logarithmic) {
        portENTER_CRITICAL(&m_mux);
        if (m_settings.logarithmic != logarithmic) { m_settings.logarithmic = logarithmic; ++m_revision; }
        portEXIT_CRITICAL(&m_mux);
    }
    void toggle() {
        portENTER_CRITICAL(&m_mux);
        m_settings.watts = m_settings.watts == 100 ? 1000 : 100;
        ++m_revision;
        portEXIT_CRITICAL(&m_mux);
    }
private:
    bool update(uint16_t watts, bool logarithmic, bool setMapping) {
        if (watts < 1 || watts > 1000) return false;
        portENTER_CRITICAL(&m_mux);
        if (m_settings.watts != watts || (setMapping && m_settings.logarithmic != logarithmic)) {
            m_settings.watts = watts;
            if (setMapping) m_settings.logarithmic = logarithmic;
            ++m_revision;
        }
        portEXIT_CRITICAL(&m_mux);
        return true;
    }
    MeterSettings m_settings;
    uint32_t m_revision = 0;
    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
};
inline void writeMeterScale(JsonDocument& doc, const MeterScale::Snapshot& scale) {
    doc["meter_scale"] = scale.watts;
    doc["meter_logarithmic"] = scale.logarithmic;
    doc["meter_scale_revision"] = scale.revision;
}
inline bool applyMeterScaleRequest(MeterScale& scale, JsonVariantConst request) {
    if (!request.is<JsonObjectConst>()) return false;
    const auto watts = request["meter_scale"];
    const auto logarithmic = request["meter_logarithmic"];
    const auto toggle = request["toggle"];
    if (!toggle.isUnbound()) {
        if (!watts.isUnbound() || !logarithmic.isUnbound() || !toggle.is<bool>() || !toggle.as<bool>()) return false;
        scale.toggle();
        return true;
    }
    if ((!watts.isUnbound() && (!watts.is<uint16_t>() || watts.as<uint16_t>() < 1 || watts.as<uint16_t>() > 1000)) ||
        (!logarithmic.isUnbound() && !logarithmic.is<bool>())) return false;
    if (!watts.isUnbound()) return logarithmic.isUnbound() ? scale.set(watts.as<uint16_t>())
        : scale.set(watts.as<uint16_t>(), logarithmic.as<bool>());
    if (logarithmic.isUnbound()) return false;
    scale.setLogarithmic(logarithmic.as<bool>());
    return true;
}
}
