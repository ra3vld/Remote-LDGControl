#pragma once
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include "tuning_types.h"
namespace ldg {
// Separate blob: configV1 and the antenna table keep their existing layouts.
struct TuningSettings {
    uint8_t managed = 0;
    uint8_t autoFallback = 0;
    uint16_t swrTenths = 20;
};
inline bool validTuningSettings(const TuningSettings& v) {
    return v.managed <= 1 && v.autoFallback <= 1 && v.swrTenths >= 11 && v.swrTenths <= 100;
}
inline bool sameTuningSettings(const TuningSettings& a, const TuningSettings& b) {
    return a.managed == b.managed && a.autoFallback == b.autoFallback && a.swrTenths == b.swrTenths;
}
struct StoredTuningSettings { uint32_t version = 1; TuningSettings value; };
template<class Storage> bool writeTuningSettings(Storage& s, const TuningSettings& v) {
    if (!validTuningSettings(v)) return false;
    StoredTuningSettings stored{}; stored.value = v;
    return s.putBytes("tuneCtlV1", &stored, sizeof(stored)) == sizeof(stored);
}
template<class Storage> bool readTuningSettings(Storage& s, TuningSettings& v) {
    StoredTuningSettings stored{};
    if (s.getBytesLength("tuneCtlV1") != sizeof(stored) ||
        s.getBytes("tuneCtlV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || !validTuningSettings(stored.value)) return false;
    v = stored.value; return true;
}
inline bool parseTuningSettings(JsonVariantConst doc, TuningSettings& v) {
    // Mode changes are commands, not an editable setting. An old form cannot
    // silently change the mode while saving its fallback threshold.
    if (!doc.is<JsonObjectConst>() || !doc["mode"].isUnbound() ||
        !doc["auto_fallback"].is<bool>() || !doc["swr_threshold"].is<double>()) return false;
    const double swr = doc["swr_threshold"].as<double>();
    if (!std::isfinite(swr) || swr < 1.1 || swr > 10.0 ||
        std::fabs(swr * 10 - std::round(swr * 10)) > 0.00001) return false;
    TuningSettings candidate = v;
    candidate.autoFallback = doc["auto_fallback"].as<bool>();
    candidate.swrTenths = static_cast<uint16_t>(std::round(swr * 10));
    if (!validTuningSettings(candidate)) return false;
    v = candidate; return true;
}
inline void writeTuningSettingsJson(JsonDocument& doc, const TuningSettings& v) {
    doc["auto_fallback"] = bool(v.autoFallback);
    doc["swr_threshold"] = v.swrTenths / 10.0;
}
}
