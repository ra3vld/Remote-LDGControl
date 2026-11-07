#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <PubSubClient.h>
#include <WiFi.h>
#include <atomic>
class NetworkClientSecure;
#include "mqtt_session.h"
#include "mqtt_status.h"

typedef bool (*mqtt_command_handler_t)(uint8_t cmd);
typedef tuner_state_t (*tuner_state_getter_t)();

class MqttHandler : private ldg::MqttTransport {
public:
    MqttHandler() = default;
    ~MqttHandler();

    bool begin(WiFiClient& wifiClient, mqtt_command_handler_t cmdHandler,
               tuner_state_getter_t stateGetter, ldg::BridgeGetter bridgeGetter=nullptr,
               ldg::BridgeHandler bridgeHandler=nullptr);
    void disconnect();
    void loop();
    bool isConnected() const;
    ldg::MqttStatus getStatus() const;
    // Safe from the web-server task; all MQTT I/O stays on the main task.
    void requestStatePublish() { m_stateRequested.store(true); }
    void requestReconfigure() { m_reconfigureRequested.store(true); }

private:
    bool configure();
    bool connect();
    bool publish(const char* topic, const char* payload, bool retained) override;
    bool subscribe(const char* topic) override;
    void packetTooLarge(const char* topic) override;
    void memoryUnavailable() override { reportError(ldg::MqttError::Memory); }
    void setConnectionState(ldg::MqttConnection state);
    void reportError(ldg::MqttError error, int code = 0, const char* topic = nullptr);
    void recoverOperation(ldg::MqttError operation, const char* topic);

    PubSubClient* m_client = nullptr;
    WiFiClient* m_wifiClient = nullptr;
    NetworkClientSecure* m_tlsClient = nullptr;
    bool m_clockRequested = false;
    mqtt_command_handler_t m_commandHandler = nullptr;
    // PubSubClient retains the hostname pointer passed to setServer().
    std::string m_broker;
    ldg::BridgeGetter m_bridgeGetter=nullptr;
    ldg::BridgeHandler m_bridgeHandler=nullptr;
    ldg::BridgeCache m_bridgeCache;
    ldg::MqttReadings m_readings;
    ldg::MqttSession* m_session = nullptr;
    ldg::MqttIdentity m_identity;
    tuner_state_getter_t m_stateGetter = nullptr;
    std::atomic<bool> m_stateRequested{false};
    std::atomic<bool> m_reconfigureRequested{false};
    mutable portMUX_TYPE m_statusMux = portMUX_INITIALIZER_UNLOCKED;
    ldg::MqttStatus m_status;
};

#endif // MQTT_HANDLER_H
