#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H
#include "config.h"
struct DeviceConfig {
    char mqttBroker[64] = "test-broker";
    uint16_t mqttPort = 1883;
    char mqttUsername[32] = "test-user";
    char mqttPassword[64] = "secret-must-not-appear";
    bool mqttDiscoveryEnabled = true;
    char webUsername[32] = "admin";
    char webPassword[64] = "test-password";
    float meterPsuVoltage = 13.8f;
};
struct FakeConfigManager {
    DeviceConfig config;
    const DeviceConfig& get() const { return config; }
};
extern FakeConfigManager configManager;
#endif
