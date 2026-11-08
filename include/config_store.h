#pragma once

#include "device_config.h"
#include <cmath>
#include <cstring>

namespace ldg {
inline bool networkSettingsChanged(const DeviceConfig& before, const DeviceConfig& after) {
    return strcmp(before.wifiSSID, after.wifiSSID) || strcmp(before.wifiPassword, after.wifiPassword) ||
        before.useStaticIP != after.useStaticIP ||
        (after.useStaticIP && (strcmp(before.staticIP, after.staticIP) ||
            strcmp(before.staticNetmask, after.staticNetmask) || strcmp(before.staticGateway, after.staticGateway) ||
            strcmp(before.staticDNS, after.staticDNS)));
}

inline bool mqttSettingsChanged(const DeviceConfig& before, const DeviceConfig& after) {
    return strcmp(before.mqttBroker, after.mqttBroker) || before.mqttPort != after.mqttPort ||
        strcmp(before.mqttUsername, after.mqttUsername) || strcmp(before.mqttPassword, after.mqttPassword) ||
        before.mqttDiscoveryEnabled != after.mqttDiscoveryEnabled;
}

template<size_t N> bool terminated(const char (&text)[N]) { return memchr(text, 0, N) != nullptr; }

inline bool validIPv4(const char* text) {
    for (int group = 0; group < 4; ++group) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + unsigned(*text++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits || (group < 3 ? *text++ != '.' : *text != 0)) return false;
    }
    return true;
}

template<class Config> inline bool validDeviceConfig(const Config& config) {
    return terminated(config.wifiSSID) && terminated(config.wifiPassword) &&
        terminated(config.mqttBroker) && terminated(config.mqttUsername) && terminated(config.mqttPassword) &&
        terminated(config.webUsername) && terminated(config.webPassword) &&
        terminated(config.ant1Name) && terminated(config.ant2Name) && terminated(config.remoteHost) &&
        terminated(config.staticIP) && terminated(config.staticNetmask) &&
        terminated(config.staticGateway) && terminated(config.staticDNS) &&
        config.webUsername[0] && config.webPassword[0] && config.mqttPort && config.displayOrientation < 16 &&
        std::isfinite(config.meterPsuVoltage) && config.meterPsuVoltage >= 8 && config.meterPsuVoltage <= 18 &&
        (!config.useStaticIP || (validIPv4(config.staticIP) && validIPv4(config.staticNetmask) &&
            validIPv4(config.staticGateway) && (!config.staticDNS[0] || validIPv4(config.staticDNS))));
}

struct StoredConfig {
    uint32_t version;
    DeviceConfig value;
};
struct StoredConfigV2 {
    uint32_t version;
    DeviceConfigV2 value;
};
static_assert(sizeof(StoredConfigV2) == 564, "Keep the legacy NVS layout readable");
static_assert(sizeof(StoredConfig) == 660, "Check stored configuration layout changes");

inline DeviceConfig migrateConfig(const DeviceConfigV2& old) {
    DeviceConfig config{};
#define LDG_COPY_TEXT(field) memcpy(config.field, old.field, sizeof(old.field))
    LDG_COPY_TEXT(wifiSSID); LDG_COPY_TEXT(wifiPassword); LDG_COPY_TEXT(mqttBroker);
    LDG_COPY_TEXT(mqttUsername); LDG_COPY_TEXT(mqttPassword);
    LDG_COPY_TEXT(webUsername); LDG_COPY_TEXT(webPassword);
    LDG_COPY_TEXT(ant1Name); LDG_COPY_TEXT(ant2Name);
    LDG_COPY_TEXT(staticIP); LDG_COPY_TEXT(staticNetmask);
    LDG_COPY_TEXT(staticGateway); LDG_COPY_TEXT(staticDNS); LDG_COPY_TEXT(remoteHost);
#undef LDG_COPY_TEXT
    config.meterPsuVoltage = old.meterPsuVoltage;
    config.mqttPort = old.mqttPort;
    config.mqttDiscoveryEnabled = old.mqttDiscoveryEnabled;
    config.remoteUnitId = old.remoteUnitId;
    config.configured = old.configured;
    config.useStaticIP = old.useStaticIP;
    config.displayOrientation = old.displayOrientation;
    return config;
}

// A single NVS blob replaces individually committed keys: a failed update
// cannot leave a mixture of old credentials and new network settings.
template<class Storage> bool writeConfig(Storage& storage, const DeviceConfig& config) {
    if (!validDeviceConfig(config)) return false;
    StoredConfig stored{};
    stored.version = 3;
    stored.value = config;
    if (storage.putBytes("configV1", &stored, sizeof(stored)) != sizeof(stored)) return false;
    StoredConfig readback{};
    return storage.getBytesLength("configV1") == sizeof(stored) &&
        storage.getBytes("configV1", &readback, sizeof(readback)) == sizeof(readback) &&
        !memcmp(&stored,&readback,sizeof(stored));
}

template<class Storage> bool readConfig(Storage& storage, DeviceConfig& config) {
    StoredConfig stored{};
    if (storage.getBytesLength("configV1") == sizeof(StoredConfigV2)) {
        StoredConfigV2 old{};
        if (storage.getBytes("configV1", &old, sizeof(old)) != sizeof(old) ||
            (old.version != 1 && old.version != 2)) return false;
        // V1's tail padding was unspecified. Never interpret it as a setting.
        if (old.version == 1) old.value.displayOrientation = 0;
        if (!validDeviceConfig(old.value)) return false;
        config = migrateConfig(old.value);
        return true;
    }
    if (storage.getBytesLength("configV1") != sizeof(stored) ||
        storage.getBytes("configV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 3) return false;
    if (!validDeviceConfig(stored.value)) return false;
    config = stored.value;
    return true;
}
}
