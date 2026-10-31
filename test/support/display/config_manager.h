#pragma once
#include "device_config.h"
struct DisplayConfigManager {
    DeviceConfig config{};
    DeviceConfig get() const { return config; }
};
extern DisplayConfigManager configManager;
