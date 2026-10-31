#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H

#include "mqtt_state.h"
#include <ArduinoJson.h>
#include <string>
#include <cstring>

namespace ldg {

static constexpr size_t MQTT_PACKET_SIZE = 1024;
static constexpr size_t MQTT_ENTITY_COUNT = 12;
static constexpr const char* HA_STATUS_TOPIC = "homeassistant/status";

struct MqttIdentity {
    std::string mac;
    std::string node;
    std::string base;
};

inline MqttIdentity mqttIdentity(const char* macAddress) {
    MqttIdentity id;
    for (const char* p = macAddress; p && *p; ++p) {
        char c = *p;
        if (c == ':' || c == '-') continue;
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        id.mac += c;
    }
    if (id.mac.size() != 12) return {};
    id.node = "ldg_" + id.mac;
    id.base = "ldg/tuner/" + id.mac;
    return id;
}

inline const char* bandName(tuner_band_t band) {
    static const char* const names[] = {
        "unknown", "160m", "80m", "60m", "40m", "30m", "20m",
        "17m", "15m", "12m", "10m", "6m"
    };
    return band > BAND_UNKNOWN && band < BAND_COUNT ? names[band] : "unknown";
}

inline tuner_band_t parseBand(const char* name) {
    if (name) {
        for (int band = BAND_160M; band < BAND_COUNT; ++band)
            if (!strcmp(name, bandName(static_cast<tuner_band_t>(band))))
                return static_cast<tuner_band_t>(band);
    }
    return BAND_UNKNOWN;
}

inline const char* modeName(tuner_mode_t mode) {
    switch (mode) {
        case MODE_AUTO: return "auto";
        case MODE_MANUAL: return "manual";
        case MODE_BYPASS: return "bypass";
        default: return "unknown";
    }
}

inline const char* antennaName(tuner_ant_t antenna) {
    return antenna == ANT_A ? "A" : antenna == ANT_B ? "B" : "unknown";
}

inline void writeState(JsonDocument& doc, const tuner_state_t& state, bool textEnums = true) {
    const tuner_meter_t& meter = state.meter;
    if (state.meterValid) {
        // ArduinoJson serializes non-finite doubles as JSON null.
        doc["fwd_power"] = meter.forward_power_watts;
        doc["ref_power"] = meter.reflected_power_watts;
        doc["swr"] = meter.swr;
        doc["fwd_raw"] = meter.forward_power_raw;
        doc["ref_raw"] = meter.reflected_power_raw;
    } else {
        doc["fwd_power"] = nullptr;
        doc["ref_power"] = nullptr;
        doc["swr"] = nullptr;
        doc["fwd_raw"] = nullptr;
        doc["ref_raw"] = nullptr;
    }
    doc["band"] = state.meterValid ? bandName(meter.band) : "unknown";
    if (textEnums) {
        doc["mode"] = modeName(state.mode);
        doc["antenna"] = antennaName(state.antenna);
    } else {
        // Preserve the web/SSE numeric enum contract.
        doc["mode"] = state.mode;
        doc["antenna"] = state.antenna;
    }
    doc["source"] = state.remote ? "remote" : "local";
}

inline tuner_state_t readWebState(JsonVariantConst doc) {
    tuner_state_t state;
    state.meterValid = doc["fwd_power"].is<double>() || doc["ref_power"].is<double>();
    const double unknown = NAN;
    state.meter.forward_power_watts = doc["fwd_power"].is<double>() ? doc["fwd_power"].as<double>() : unknown;
    state.meter.reflected_power_watts = doc["ref_power"].is<double>() ? doc["ref_power"].as<double>() : unknown;
    state.meter.swr = doc["swr"].is<double>() ? doc["swr"].as<double>() : unknown;
    state.meter.forward_power_raw = doc["fwd_raw"] | 0;
    state.meter.reflected_power_raw = doc["ref_raw"] | 0;
    state.meter.band = parseBand(doc["band"].as<const char*>());
    const int mode = doc["mode"].is<int>() ? doc["mode"].as<int>() : MODE_UNKNOWN;
    const int antenna = doc["antenna"].is<int>() ? doc["antenna"].as<int>() : ANT_UNKNOWN;
    if (mode >= MODE_UNKNOWN && mode <= MODE_BYPASS) state.mode = static_cast<tuner_mode_t>(mode);
    if (antenna >= ANT_UNKNOWN && antenna <= ANT_B) state.antenna = static_cast<tuner_ant_t>(antenna);
    return state;
}

struct MqttEntity {
    const char* key;
    const char* name;
    const char* command;
};

inline const MqttEntity& mqttEntity(size_t index) {
    static const MqttEntity entities[MQTT_ENTITY_COUNT] = {
        {"fwd_power", "Forward power", nullptr},
        {"ref_power", "Reflected power", nullptr},
        {"swr", "SWR", nullptr},
        {"band", "Band", nullptr},
        {"mode", "Mode", nullptr},
        {"antenna", "Antenna", nullptr},
        {"memory_tune", "Memory tune", "memory_tune"},
        {"full_tune", "Full tune", "full_tune"},
        {"bypass", "Bypass", "bypass"},
        {"toggle", "Toggle antenna", "toggle"},
        {"auto", "Auto mode", "auto"},
        {"manual", "Manual mode", "manual"}
    };
    return entities[index];
}

inline std::string discoveryTopic(const MqttIdentity& id, size_t index) {
    const MqttEntity& entity = mqttEntity(index);
    return std::string("homeassistant/") + (entity.command ? "button/" : "sensor/") +
        id.node + "/" + entity.key + "/config";
}

inline void writeDiscovery(JsonDocument& doc, const MqttIdentity& id, size_t index,
                           const char* model, const char* configurationUrl) {
    const MqttEntity& entity = mqttEntity(index);
    doc["name"] = entity.name;
    doc["unique_id"] = id.node + "_" + entity.key;
    doc["availability_topic"] = id.base + "/status";
    doc["qos"] = 0;
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(id.node);
    device["name"] = "LDG Tuner " + id.mac.substr(6);
    device["manufacturer"] = "Remote-LDGControl";
    device["model"] = model;
    device["configuration_url"] = configurationUrl;
    doc["origin"]["name"] = "Remote-LDGControl";
    if (entity.command) {
        doc["command_topic"] = id.base + "/command";
        JsonDocument command;
        command["command"] = entity.command;
        std::string payload;
        serializeJson(command, payload);
        doc["payload_press"] = payload;
        doc["retain"] = false;
    } else {
        doc["state_topic"] = id.base + "/state";
        doc["value_template"] = std::string("{{ value_json.") + entity.key + " }}";
        if (index < 4) {
            doc["json_attributes_topic"] = id.base + "/state";
            doc["json_attributes_template"] = std::string("{{ {'held': value_json.get('held', {}).get('") +
                entity.key + "', true), 'source': value_json.get('source', 'unknown')} | to_json }}";
        }
        if (index < 2) {
            doc["device_class"] = "power";
            doc["unit_of_measurement"] = "W";
            doc["state_class"] = "measurement";
        } else if (index == 2) {
            doc["state_class"] = "measurement";
        }
    }
}

inline bool mqttPacketFits(const std::string& topic, size_t payloadSize) {
    return 5 + 2 + topic.size() + payloadSize <= MQTT_PACKET_SIZE;
}

struct MqttCommand {
    std::string name;
    uint8_t byte = 0;
    const char* error = nullptr;
};

inline MqttCommand parseCommand(const uint8_t* payload, size_t length) {
    MqttCommand command;
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
        command.error = "invalid_json";
        return command;
    }
    if (!doc["command"].is<const char*>()) {
        command.error = "missing_command";
        return command;
    }
    command.name = doc["command"].as<const char*>();
    if (command.name == "antenna_1" || command.name == "antenna_2") {
        command.byte = command.name == "antenna_1" ? TUNER_CMD_SELECT_ANT_A : TUNER_CMD_SELECT_ANT_B;
        return command;
    }
    static const uint8_t commands[] = {
        TUNER_CMD_MEM_TUNE, TUNER_CMD_FULL_TUNE, TUNER_CMD_BYPASS,
        TUNER_CMD_TOGGLE_ANT, TUNER_CMD_AUTO_MODE, TUNER_CMD_MANUAL_MODE
    };
    for (size_t i = 6; i < MQTT_ENTITY_COUNT; ++i) {
        if (command.name == mqttEntity(i).command) {
            command.byte = commands[i - 6];
            return command;
        }
    }
    command.error = "unknown_command";
    return command;
}

} // namespace ldg
#endif // MQTT_PROTOCOL_H
