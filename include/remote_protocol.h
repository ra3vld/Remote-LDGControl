#pragma once

#include "mqtt_protocol.h"
#include "config.h"
#include <cstring>

namespace ldg {
inline uint16_t remoteRequestTimeout(uint8_t command) {
    // Include optional delayed Z, UART mode switches/guards, and network margin.
    const uint16_t overhead = TUNER_SYNC_TIMEOUT_MS + 4 * TUNER_CMD_DELAY_MS + 2000;
    if (!command) return 3000;
    if (command == TUNER_CMD_MEM_TUNE || command == TUNER_CMD_FULL_TUNE)
        return TUNER_TUNE_TIMEOUT_MS + overhead;
    if (command == TUNER_CMD_SELECT_ANT_A || command == TUNER_CMD_SELECT_ANT_B)
        return 2 * (TUNER_ANTENNA_TIMEOUT_MS + 4 * TUNER_CMD_DELAY_MS) + TUNER_SYNC_TIMEOUT_MS + 2000;
    return TUNER_MODE_TIMEOUT_MS + overhead;
}

inline const char* commandEndpoint(uint8_t command) {
    switch (command) {
        case TUNER_CMD_TOGGLE_ANT: return "/api/command/toggle";
        case TUNER_CMD_SELECT_ANT_A: return "/api/command/antenna_1";
        case TUNER_CMD_SELECT_ANT_B: return "/api/command/antenna_2";
        case TUNER_CMD_MEM_TUNE: return "/api/command/memory_tune";
        case TUNER_CMD_FULL_TUNE: return "/api/command/full_tune";
        case TUNER_CMD_BYPASS: return "/api/command/bypass";
        case TUNER_CMD_AUTO_MODE: return "/api/command/auto";
        case TUNER_CMD_MANUAL_MODE: return "/api/command/manual";
        case TUNER_CMD_CONTROLLED_MODE: return "/api/command/controlled";
        default: return nullptr;
    }
}

inline bool remoteCommandSucceeded(int status, JsonVariantConst response) {
    const char* result = response["result"].as<const char*>();
    return status == 200 && result && !strcmp(result, "success");
}

inline bool validRemoteStatus(JsonVariantConst response) {
    // Refuse a peer that is itself proxying, so two displays cannot form a loop.
    const char* source = response["source"].as<const char*>();
    return response.is<JsonObjectConst>() && !response["fwd_power"].isUnbound() &&
        !response["ref_power"].isUnbound() && (!source || strcmp(source, "remote"));
}
}
