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

inline bool validDeviceConfig(const DeviceConfig& config) {
    return terminated(config.wifiSSID) && terminated(config.wifiPassword) &&
        terminated(config.mqttBroker) && terminated(config.mqttUsername) && terminated(config.mqttPassword) &&
        terminated(config.webUsername) && terminated(config.webPassword) &&
        terminated(config.ant1Name) && terminated(config.ant2Name) && terminated(config.remoteHost) &&
        terminated(config.staticIP) && terminated(config.staticNetmask) &&
        terminated(config.staticGateway) && terminated(config.staticDNS) &&
        config.webUsername[0] && config.webPassword[0] && config.mqttPort &&
        std::isfinite(config.meterPsuVoltage) && config.meterPsuVoltage >= 8 && config.meterPsuVoltage <= 18 &&
        (!config.useStaticIP || (validIPv4(config.staticIP) && validIPv4(config.staticNetmask) &&
            validIPv4(config.staticGateway) && (!config.staticDNS[0] || validIPv4(config.staticDNS))));
}

struct StoredConfig {
    uint32_t version;
    DeviceConfig value;
};

// A single NVS blob replaces individually committed keys: a failed update
// cannot leave a mixture of old credentials and new network settings.
template<class Storage> bool writeConfig(Storage& storage, const DeviceConfig& config) {
    if (!validDeviceConfig(config)) return false;
    StoredConfig stored{};
    stored.version = 1;
    stored.value = config;
    return storage.putBytes("configV1", &stored, sizeof(stored)) == sizeof(stored);
}

template<class Storage> bool readConfig(Storage& storage, DeviceConfig& config) {
    StoredConfig stored{};
    if (storage.getBytesLength("configV1") != sizeof(stored) ||
        storage.getBytes("configV1", &stored, sizeof(stored)) != sizeof(stored) ||
        stored.version != 1 || !validDeviceConfig(stored.value)) return false;
    config = stored.value;
    return true;
}
}
