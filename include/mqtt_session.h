#ifndef MQTT_SESSION_H
#define MQTT_SESSION_H

#include "mqtt_readings.h"
#include "config.h"

namespace ldg {

// The same session runs on PubSubClient and the host-test broker.
class MqttTransport {
public:
    virtual ~MqttTransport() = default;
    virtual bool publish(const char* topic, const char* payload, bool retained) = 0;
    virtual bool subscribe(const char* topic) = 0;
    virtual void packetTooLarge(const char* topic) { (void)topic; }
    virtual void memoryUnavailable() {}
};

class MqttSession {
public:
    MqttSession(MqttTransport& transport, const MqttIdentity& identity,
                bool (*commandHandler)(uint8_t), uint32_t (*clock)() = nullptr,
                MqttReadings* readings = nullptr)
        : m_transport(transport), m_identity(identity), m_commandHandler(commandHandler), m_clock(clock),
          m_readings(readings ? readings : &m_ownedReadings),
          m_stateTopic(identity.base + "/state"), m_commandTopic(identity.base + "/command"),
          m_statusTopic(identity.base + "/status"), m_responseTopic(identity.base + "/command/response") {}
    MqttSession(const MqttSession&) = delete;
    MqttSession& operator=(const MqttSession&) = delete;

    bool connected(bool discoveryEnabled, const char* model, const char* configurationUrl) {
        m_discoveryEnabled = discoveryEnabled;
        m_model = model;
        m_configurationUrl = configurationUrl;
        m_discoveryIndex = 0;
        m_schedule.requestImmediate();
        return m_transport.subscribe(m_commandTopic.c_str()) &&
            (!m_discoveryEnabled || m_transport.subscribe(HA_STATUS_TOPIC)) &&
            m_transport.publish(m_statusTopic.c_str(), "online", true);
    }

    void requestImmediate() { m_schedule.requestImmediate(); }

    void loop(uint32_t now, const tuner_state_t& state) {
        m_readings->observe(state);
        if (m_discoveryIndex < MQTT_ENTITY_COUNT) {
            JsonDocument doc;
            if (m_discoveryEnabled)
                writeDiscovery(doc, m_identity, m_discoveryIndex,
                               m_model.c_str(), m_configurationUrl.c_str());
            const std::string topic = discoveryTopic(m_identity, m_discoveryIndex);
            if (m_discoveryEnabled ? !sendDocument(topic, doc, true)
                                   : !m_transport.publish(topic.c_str(), "", true)) return;
            ++m_discoveryIndex;
            if (m_discoveryIndex < MQTT_ENTITY_COUNT) return;
        }
        if (m_schedule.due(now, state)) {
            JsonDocument doc;
            m_readings->write(doc, state);
            if (sendDocument(m_stateTopic, doc, MQTT_RETAIN_TELEMETRY))
                m_schedule.published(m_clock ? m_clock() : now, state);
        }
    }

    void message(const char* topic, const uint8_t* payload, size_t length) {
        if (m_discoveryEnabled && !strcmp(topic, HA_STATUS_TOPIC)) {
            if (length == 6 && !memcmp(payload, "online", 6)) {
                m_discoveryIndex = 0;
                m_schedule.requestImmediate();
            }
            return;
        }
        if (m_commandTopic != topic) return;
        const MqttCommand command = parseCommand(payload, length);
        const char* result = command.error;
        if (!result) {
            result = m_commandHandler && m_commandHandler(command.byte) ? "success" : "failed";
            m_schedule.requestImmediate();
        }
        JsonDocument response;
        response["command"] = command.name;
        response["result"] = result;
        sendDocument(m_responseTopic, response, false);
    }

    void disconnect() {
        m_transport.publish(m_statusTopic.c_str(), "offline", true);
    }

private:
    bool sendDocument(const std::string& topic, const JsonDocument& doc, bool retained) {
        if (doc.overflowed()) {
            m_transport.memoryUnavailable();
            return false;
        }
        const size_t length = measureJson(doc);
        if (length >= sizeof(m_payload) || !mqttPacketFits(topic, length)) {
            m_transport.packetTooLarge(topic.c_str());
            return false;
        }
        serializeJson(doc, m_payload, sizeof(m_payload));
        return m_transport.publish(topic.c_str(), m_payload, retained);
    }

    MqttTransport& m_transport;
    MqttIdentity m_identity;
    bool (*m_commandHandler)(uint8_t);
    uint32_t (*m_clock)();
    MqttReadings m_ownedReadings;
    MqttReadings* m_readings;
    MqttStateSchedule m_schedule;
    bool m_discoveryEnabled = true;
    size_t m_discoveryIndex = MQTT_ENTITY_COUNT;
    std::string m_model;
    std::string m_configurationUrl;
    const std::string m_stateTopic, m_commandTopic, m_statusTopic, m_responseTopic;
    char m_payload[MQTT_PACKET_SIZE]{};
};

} // namespace ldg
#endif // MQTT_SESSION_H
