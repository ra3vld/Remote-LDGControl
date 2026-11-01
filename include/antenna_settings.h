#pragma once

#include "tuner_types.h"
#include <ArduinoJson.h>
#include <cstring>

namespace ldg {
// Stored separately from configV1: adding a band table must not invalidate
// the existing binary DeviceConfig (network credentials, passwords, etc.).
struct AntennaSettings {
    uint8_t enabled = 0;
    uint8_t bands[BAND_COUNT] = {};
};
inline bool validAntennaSettings(const AntennaSettings& value) {
    if (value.enabled > 1 || value.bands[BAND_UNKNOWN] != ANT_UNKNOWN) return false;
    for (unsigned i = 1; i < BAND_COUNT; ++i) if (value.bands[i] > ANT_B) return false;
    return true;
}
inline bool sameAntennaSettings(const AntennaSettings& a, const AntennaSettings& b) {
    return a.enabled == b.enabled && !memcmp(a.bands, b.bands, sizeof(a.bands));
}
struct StoredAntennaSettings {
    uint32_t version = 1;
    AntennaSettings value;
};
template<class Storage> bool writeAntennaSettings(Storage& storage, const AntennaSettings& value) {
    if (!validAntennaSettings(value)) return false;
    StoredAntennaSettings stored{};
    stored.value = value;
    return storage.putBytes("antBandsV1", &stored, sizeof(stored)) == sizeof(stored);
}
template<class Storage> bool readAntennaSettings(Storage& storage, AntennaSettings& value) {
    StoredAntennaSettings stored{};
    if (storage.getBytesLength("antBandsV1") != sizeof(stored) ||
        storage.getBytes("antBandsV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || !validAntennaSettings(stored.value)) return false;
    value = stored.value;
    return true;
}
// An entire table is replaced atomically; malformed or partial tables fail.
inline bool parseAntennaSettings(JsonVariantConst doc, AntennaSettings& value) {
    if (!doc.is<JsonObjectConst>() || !doc["enabled"].is<bool>() ||
        !doc["bands"].is<JsonArrayConst>() || doc["bands"].size() != BAND_COUNT - 1) return false;
    AntennaSettings candidate{};
    candidate.enabled = doc["enabled"].as<bool>();
    for (unsigned i = 1; i < BAND_COUNT; ++i) {
        const auto item = doc["bands"][i - 1];
        if (!item.is<unsigned>() || item.as<unsigned>() > ANT_B) return false;
        candidate.bands[i] = item.as<unsigned>();
    }
    value = candidate;
    return true;
}
inline void writeAntennaSettingsJson(JsonDocument& doc, const AntennaSettings& value) {
    doc["enabled"] = bool(value.enabled);
    auto bands = doc["bands"].to<JsonArray>();
    for (unsigned i = 1; i < BAND_COUNT; ++i) bands.add(value.bands[i]);
}
}
