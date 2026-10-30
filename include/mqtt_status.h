#ifndef MQTT_STATUS_H
#define MQTT_STATUS_H

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ldg {

static constexpr uint32_t MQTT_RECONNECT_MS = 5000;

enum class MqttConnection { Disabled, WaitingWifi, Connecting, Connected, Disconnected, Error };
enum class MqttError { None, Identity, Memory, Connect, ConnectionLost, WifiLost, Publish, Subscribe, PacketSize };

inline const char* mqttConnectionName(MqttConnection state) {
    switch (state) {
        case MqttConnection::Disabled: return "disabled";
        case MqttConnection::WaitingWifi: return "waiting_wifi";
        case MqttConnection::Connecting: return "connecting";
        case MqttConnection::Connected: return "connected";
        case MqttConnection::Disconnected: return "disconnected";
        default: return "error";
    }
}

inline const char* mqttConnectionMessage(MqttConnection state) {
    switch (state) {
        case MqttConnection::Disabled: return "Disabled (broker not configured)";
        case MqttConnection::WaitingWifi: return "Waiting for Wi-Fi";
        case MqttConnection::Connecting: return "Connecting";
        case MqttConnection::Connected: return "Connected";
        case MqttConnection::Disconnected: return "Disconnected; waiting to reconnect";
        default: return "Initialization failed";
    }
}

// PubSubClient 2.8 state codes. A TCP failure does not distinguish DNS,
// routing, refusal, or a TCP timeout; do not claim a more specific cause.
inline const char* mqttClientStateMessage(int code) {
    switch (code) {
        case -4: return "Broker response / keepalive timeout";
        case -3: return "Network connection lost";
        case -2: return "TCP connection failed (check broker address, DNS, port, and network)";
        case -1: return "Client disconnected";
        case 0: return "Connected";
        case 1: return "Broker rejected MQTT protocol version";
        case 2: return "Broker rejected client ID";
        case 3: return "Broker unavailable";
        case 4: return "Broker rejected username or password";
        case 5: return "Broker denied authorization";
        default: return "Unknown MQTT error";
    }
}

inline const char* mqttErrorMessage(MqttError error) {
    switch (error) {
        case MqttError::Identity: return "Cannot generate MQTT identity from MAC address";
        case MqttError::Memory: return "Not enough memory to initialize MQTT";
        case MqttError::Connect: return "Connection failed";
        case MqttError::ConnectionLost: return "Connection lost";
        case MqttError::WifiLost: return "Wi-Fi disconnected";
        case MqttError::Publish: return "Publish failed (transport write failed)";
        case MqttError::Subscribe: return "Subscription request could not be sent";
        case MqttError::PacketSize: return "MQTT packet exceeds the 1024-byte buffer";
        default: return "";
    }
}

// Value-only snapshot. The ESP32 owner protects reads/writes across tasks.
struct MqttStatus {
    MqttConnection state = MqttConnection::Disabled;
    MqttError lastError = MqttError::None;
    int errorCode = 0;
    bool errorActive = false;
    char errorTopic[128] = {};
    uint32_t lastErrorMs = 0;
    uint32_t attempts = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t connectedAtMs = 0;

    bool failed(MqttError error, int code, const char* topic, uint32_t now) {
        const char* name = topic ? topic : "";
        const bool changed = !errorActive || lastError != error || errorCode != code ||
                             strcmp(errorTopic, name) != 0;
        lastError = error;
        errorCode = code;
        errorActive = true;
        snprintf(errorTopic, sizeof(errorTopic), "%s", name);
        lastErrorMs = now;
        return changed;
    }

    bool recover(MqttError operation, const char* topic) {
        if (!errorActive || lastError != operation ||
            strcmp(errorTopic, topic ? topic : "") != 0) return false;
        errorActive = false;
        return true;
    }

    void connected(uint32_t now) {
        state = MqttConnection::Connected;
        connectedAtMs = now;
        errorActive = false; // Preserve the last failure as history after recovery.
    }
};

inline void writeMqttStatus(JsonObject out, const MqttStatus& status, uint32_t now) {
    out["state"] = mqttConnectionName(status.state);
    out["message"] = mqttConnectionMessage(status.state);
    out["enabled"] = status.state != MqttConnection::Disabled;
    out["connected"] = status.state == MqttConnection::Connected;
    out["attempts"] = status.attempts;
    out["errorActive"] = status.errorActive;
    const uint32_t elapsed = now - status.lastAttemptMs;
    out["retryInMs"] = status.state == MqttConnection::Disconnected && elapsed < MQTT_RECONNECT_MS
        ? MQTT_RECONNECT_MS - elapsed : 0;
    if (status.state == MqttConnection::Connected)
        out["connectedForMs"] = now - status.connectedAtMs;
    if (status.lastError != MqttError::None) {
        JsonObject error = out["lastError"].to<JsonObject>();
        error["message"] = mqttErrorMessage(status.lastError);
        if (status.lastError == MqttError::Connect || status.lastError == MqttError::ConnectionLost ||
            status.lastError == MqttError::Publish || status.lastError == MqttError::Subscribe) {
            error["code"] = status.errorCode;
            if (status.errorCode != 0)
                error["detail"] = mqttClientStateMessage(status.errorCode);
        }
        if (status.errorTopic[0]) error["topic"] = status.errorTopic;
        error["ageMs"] = now - status.lastErrorMs;
    } else {
        out["lastError"] = nullptr;
    }
}

} // namespace ldg
#endif // MQTT_STATUS_H
