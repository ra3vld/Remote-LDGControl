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
                MqttReadings* readings = nullptr, BridgeGetter bridgeGetter = nullptr,
                BridgeHandler bridgeHandler = nullptr, BridgeCache* cache = nullptr, tuner_state_t (*stateGetter)() = nullptr)
        : m_transport(transport), m_identity(identity), m_commandHandler(commandHandler), m_clock(clock),
          m_readings(readings ? readings : &m_ownedReadings),
          m_bridgeGetter(bridgeGetter),m_bridgeHandler(bridgeHandler),m_cache(cache),m_stateGetter(stateGetter),
          m_stateTopic(identity.base + "/state"), m_commandTopic(identity.base + "/command"),
          m_statusTopic(identity.base + "/status"), m_responseTopic(identity.base + "/command/response"),m_radioTopic(identity.base + "/radio/state") {}
    MqttSession(const MqttSession&) = delete;
    MqttSession& operator=(const MqttSession&) = delete;

    bool connected(bool discoveryEnabled, const char* model, const char* configurationUrl) {
        m_discoveryEnabled = discoveryEnabled;
        m_model = model;
        m_configurationUrl = configurationUrl;
        m_discoveryIndex = 0;
        m_schedule.requestImmediate();
        m_radioPublished=false;
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
            const auto context=m_bridgeGetter ? m_bridgeGetter() : BridgeState{};
            // A source may switch between the value snapshot and metadata read.
            // Drop that frame; never grant a local token alongside remote values.
            if (m_bridgeGetter && context.remote!=state.remote) return;
            if (m_bridgeGetter) writeBridgeState(doc,context);
            if (sendDocument(m_stateTopic, doc, MQTT_RETAIN_TELEMETRY))
                m_schedule.published(m_clock ? m_clock() : now, state);
        }
        if (m_bridgeGetter && (!m_radioPublished || (state.radio.provider != RadioProvider::None && now-m_radioAt>=1000) || state.radio.provider!=m_radioProvider)) {
            JsonDocument doc;
            writeRadioJson(doc.to<JsonObject>(),state.radio);
            doc.remove("free_heap"); doc.remove("largest_block"); doc.remove("stack_free");
            if (!state.radio.xitKnown) doc["xit_offset_hz"]=nullptr;
            const auto context=m_bridgeGetter();
            if (context.remote!=state.remote) return;
            doc["source_token"]=context.sourceToken;
            if (sendDocument(m_radioTopic,doc,true)) { m_radioPublished=true; m_radioAt=now; m_radioProvider=state.radio.provider; }
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
        if (length>MQTT_PACKET_SIZE) return;
        MqttCommand command = parseCommand(payload, length);
        const char* result = command.error;
        if (!result && command.extended) {
            command.request.command=command.byte;
            result=m_cache ? m_cache->find(command.request) : "invalid_request";
            if (!result) {
                result=m_bridgeGetter ? validateBridgeRequest(command.request,m_bridgeGetter()) : "invalid_request";
                if (!result && m_bridgeGetter().busy) result="busy";
                if (!result) {
                    // Keep controller liveness valid during a long synchronous tune.
                    if (m_bridgeGetter && m_stateGetter) {
                        const auto current=m_stateGetter(); auto context=m_bridgeGetter(); context.busy=true;
                        if (context.remote==current.remote) {
                            JsonDocument state; m_readings->observe(current); m_readings->write(state,current);
                            writeBridgeState(state,context); sendDocument(m_stateTopic,state,true);
                        }
                    }
                    result=m_bridgeHandler ? m_bridgeHandler(command.request) : "invalid_request";
                }
                m_cache->save(command.request,result);
            }
            m_schedule.requestImmediate();
        } else if (!result) {
            result = m_commandHandler && m_commandHandler(command.byte) ? "success" : "failed";
            m_schedule.requestImmediate();
        }
        JsonDocument response;
        if (command.extended && command.request.requestId[0]) {
            response["request_id"]=command.request.requestId;
            response["source_token"]=command.request.sourceToken;
        }
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
    BridgeGetter m_bridgeGetter;
    BridgeHandler m_bridgeHandler;
    BridgeCache* m_cache;
    tuner_state_t (*m_stateGetter)();
    uint32_t m_radioAt=0;
    bool m_radioPublished=false;
    RadioProvider m_radioProvider=RadioProvider::None;
    MqttStateSchedule m_schedule;
    bool m_discoveryEnabled = true;
    size_t m_discoveryIndex = MQTT_ENTITY_COUNT;
    std::string m_model;
    std::string m_configurationUrl;
    const std::string m_stateTopic, m_commandTopic, m_statusTopic, m_responseTopic, m_radioTopic;
    char m_payload[MQTT_PACKET_SIZE]{};
};

} // namespace ldg
#endif // MQTT_SESSION_H
