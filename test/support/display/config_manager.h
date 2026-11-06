#pragma once
#include "device_config.h"
struct DisplayConfigManager {
    DeviceConfig config{};
    DeviceConfig get() const { return config; }
    uint8_t displayOrientation() const { return config.displayOrientation; }
};
extern DisplayConfigManager configManager;
