#pragma once

#include <stdint.h>
#include <cstring>
#include <ArduinoJson.h>

namespace ldg {
struct RadioSettings {
    uint8_t enabled = 0;
    char host[64]{};
    uint16_t port = 40001;
    uint8_t receiver = 0;
};
inline bool validRadioSettings(const RadioSettings& value) {
    if (value.enabled > 1 || !value.port || value.receiver >= 16 ||
        !memchr(value.host, 0, sizeof(value.host)) || (value.enabled && !value.host[0])) return false;
    for (const char* p = value.host; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' )) return false;
    return true;
}
inline bool sameRadioSettings(const RadioSettings& a, const RadioSettings& b) {
    return a.enabled == b.enabled && a.port == b.port && a.receiver == b.receiver && !strcmp(a.host, b.host);
}
struct StoredRadioSettings { uint32_t version; RadioSettings value; };
template<class Storage> bool writeRadioSettings(Storage& storage, const RadioSettings& value) {
    if (!validRadioSettings(value)) return false;
    StoredRadioSettings stored{};
    stored.version = 1; stored.value = value;
    return storage.putBytes("radioV1", &stored, sizeof(stored)) == sizeof(stored);
}
template<class Storage> bool readRadioSettings(Storage& storage, RadioSettings& value) {
    StoredRadioSettings stored{};
    if (storage.getBytesLength("radioV1") != sizeof(stored) ||
        storage.getBytes("radioV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || !validRadioSettings(stored.value)) return false;
    value = stored.value;
    return true;
}
inline bool parseRadioSettings(JsonVariantConst doc, RadioSettings& value) {
    if (!doc.is<JsonObjectConst>() || !doc["enabled"].is<bool>() || !doc["host"].is<const char*>() ||
        !doc["port"].is<unsigned>() || doc["port"].as<unsigned>() > 65535 ||
        !doc["receiver"].is<unsigned>() || doc["receiver"].as<unsigned>() >= 16) return false;
    RadioSettings candidate{};
    const char* host = doc["host"];
    if (strlen(host) >= sizeof(candidate.host)) return false;
    strcpy(candidate.host, host);
    candidate.enabled = doc["enabled"].as<bool>();
    candidate.port = doc["port"].as<unsigned>();
    candidate.receiver = doc["receiver"].as<unsigned>();
    if (!validRadioSettings(candidate)) return false;
    value = candidate; return true;
}
inline void writeRadioSettingsJson(JsonDocument& doc, const RadioSettings& value) {
    doc["enabled"] = bool(value.enabled); doc["host"] = value.host;
    doc["port"] = value.port; doc["receiver"] = value.receiver;
}
}
