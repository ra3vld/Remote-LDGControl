#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <PubSubClient.h>
#include <WiFi.h>
#include <atomic>
#include "mqtt_session.h"

typedef bool (*mqtt_command_handler_t)(uint8_t cmd);
typedef tuner_state_t (*tuner_state_getter_t)();

class MqttHandler : private ldg::MqttTransport {
public:
    MqttHandler() = default;
    ~MqttHandler();

    bool begin(WiFiClient& wifiClient, mqtt_command_handler_t cmdHandler,
               tuner_state_getter_t stateGetter);
    void disconnect();
    void loop();
    bool isConnected() const;
    // Safe from the web-server task; all MQTT I/O stays on the main task.
    void requestStatePublish() { m_stateRequested.store(true); }

private:
    bool connect();
    bool publish(const char* topic, const char* payload, bool retained) override;
    bool subscribe(const char* topic) override;

    PubSubClient* m_client = nullptr;
    ldg::MqttSession* m_session = nullptr;
    ldg::MqttIdentity m_identity;
    tuner_state_getter_t m_stateGetter = nullptr;
    std::atomic<bool> m_stateRequested{false};
    uint32_t m_lastReconnectAttempt = 0;
};

#endif // MQTT_HANDLER_H
