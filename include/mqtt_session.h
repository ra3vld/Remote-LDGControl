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
};

class MqttSession {
public:
    MqttSession(MqttTransport& transport, const MqttIdentity& identity,
                bool (*commandHandler)(uint8_t), uint32_t (*clock)() = nullptr,
                MqttReadings* readings = nullptr)
        : m_transport(transport), m_identity(identity), m_commandHandler(commandHandler), m_clock(clock),
          m_readings(readings ? readings : &m_ownedReadings) {}
    MqttSession(const MqttSession&) = delete;
    MqttSession& operator=(const MqttSession&) = delete;

    bool connected(bool discoveryEnabled, const char* model, const char* configurationUrl) {
        m_discoveryEnabled = discoveryEnabled;
        m_model = model;
        m_configurationUrl = configurationUrl;
        m_discoveryIndex = 0;
        m_schedule.requestImmediate();
        return m_transport.subscribe((m_identity.base + "/command").c_str()) &&
            (!m_discoveryEnabled || m_transport.subscribe(HA_STATUS_TOPIC)) &&
            m_transport.publish((m_identity.base + "/status").c_str(), "online", true);
    }

    void requestImmediate() { m_schedule.requestImmediate(); }

    void loop(uint32_t now, const tuner_state_t& state) {
        m_readings->observe(state);
        if (m_discoveryIndex < MQTT_ENTITY_COUNT) {
            JsonDocument doc;
            if (m_discoveryEnabled)
                writeDiscovery(doc, m_identity, m_discoveryIndex,
                               m_model.c_str(), m_configurationUrl.c_str());
            std::string payload;
            if (m_discoveryEnabled) serializeJson(doc, payload);
            const std::string topic = discoveryTopic(m_identity, m_discoveryIndex);
            if (!send(topic, payload, true)) return;
            ++m_discoveryIndex;
            if (m_discoveryIndex < MQTT_ENTITY_COUNT) return;
        }
        if (m_schedule.due(now, state)) {
            JsonDocument doc;
            m_readings->write(doc, state);
            std::string payload;
            serializeJson(doc, payload);
            if (send(m_identity.base + "/state", payload, MQTT_RETAIN_TELEMETRY))
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
        if (m_identity.base + "/command" != topic) return;
        const MqttCommand command = parseCommand(payload, length);
        const char* result = command.error;
        if (!result) {
            result = m_commandHandler && m_commandHandler(command.byte) ? "success" : "failed";
            m_schedule.requestImmediate();
        }
        JsonDocument response;
        response["command"] = command.name;
        response["result"] = result;
        std::string out;
        serializeJson(response, out);
        send(m_identity.base + "/command/response", out, false);
    }

    void disconnect() {
        m_transport.publish((m_identity.base + "/status").c_str(), "offline", true);
    }

private:
    bool send(const std::string& topic, const std::string& payload, bool retained) {
        if (!mqttPacketFits(topic, payload.size())) {
            m_transport.packetTooLarge(topic.c_str());
            return false;
        }
        return m_transport.publish(topic.c_str(), payload.c_str(), retained);
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
};

} // namespace ldg
#endif // MQTT_SESSION_H
