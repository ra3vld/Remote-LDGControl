#pragma once

#include "mqtt_protocol.h"
#include <cstring>

namespace ldg {
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
