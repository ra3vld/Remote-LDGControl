#pragma once
#include "tuning_settings.h"
#include "tuner_types.h"

namespace ldg {
// Shared by queued display/web/MQTT commands and the direct web fallback.
// Save the selected profile before any UART write. A failed save leaves both
// the current mode and the running policy intact.
template<class Config, class Tuner>
bool selectOperatingMode(Config& config, Tuner& tuner, uint8_t command) {
    if (!isModeCommand(command) || !config.updateTuningMode(command == TUNER_CMD_CONTROLLED_MODE)) return false;
    tuner.manualCommandStarted();
    tuner.configureTuning(config.tuningSettings());
    if (command == TUNER_CMD_CONTROLLED_MODE) return tuner.setControlledMode();
    return command == TUNER_CMD_MANUAL_MODE ? tuner.setManualMode() : tuner.setAutoMode();
}
}
