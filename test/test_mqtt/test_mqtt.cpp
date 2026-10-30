#include <unity.h>
#include "mqtt_session.h"
#include <map>
#include <set>
#include <vector>
#include <limits>

using namespace ldg;

void setUp() {}
void tearDown() {}

static tuner_state_t measured(double forward = 100, double reflected = 1, double swr = 1.5) {
    tuner_state_t state;
    state.meterValid = true;
    state.meter.forward_power_watts = forward;
    state.meter.reflected_power_watts = reflected;
    state.meter.swr = swr;
    state.meter.band = BAND_20M;
    state.mode = MODE_AUTO;
    state.antenna = ANT_A;
    return state;
}

void test_idle_interval_including_zero() {
    for (const auto& state : {measured(), measured(0, 0, 1.0), tuner_state_t{}}) {
        MqttStateSchedule schedule;
        TEST_ASSERT_TRUE(schedule.due(0, state));
        schedule.published(0, state);
        TEST_ASSERT_FALSE(schedule.due(999, state));
        TEST_ASSERT_FALSE(schedule.due(1000, state));
        TEST_ASSERT_FALSE(schedule.due(29999, state));
        TEST_ASSERT_TRUE(schedule.due(30000, state));
        schedule.published(30000, state);
        TEST_ASSERT_FALSE(schedule.due(59999, state));
        TEST_ASSERT_TRUE(schedule.due(60000, state));
    }
}

void test_absolute_and_relative_threshold_boundaries() {
    const auto previous = measured();
    auto state = previous;
    state.meter.forward_power_watts += 0.999;
    TEST_ASSERT_FALSE(significantChange(previous, state));
    state.meter.forward_power_watts = 101;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    auto high = measured(1000, 100);
    state = high;
    state.meter.forward_power_watts = 1009.999;
    TEST_ASSERT_FALSE(significantChange(high, state));
    state.meter.forward_power_watts = 1010;
    TEST_ASSERT_TRUE(significantChange(high, state));
    state = previous;
    state.meter.reflected_power_watts += 0.0999;
    TEST_ASSERT_FALSE(significantChange(previous, state));
    state.meter.reflected_power_watts = 1.1;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    state = high;
    state.meter.reflected_power_watts = 100.999;
    TEST_ASSERT_FALSE(significantChange(high, state));
    state.meter.reflected_power_watts = 101;
    TEST_ASSERT_TRUE(significantChange(high, state));
    state = measured(100, 1, 1.1);
    auto next = state;
    next.meter.swr = 1.1499;
    TEST_ASSERT_FALSE(significantChange(state, next));
    next.meter.swr = 1.15;
    TEST_ASSERT_TRUE(significantChange(state, next));
    next.meter.swr = 1.05;
    TEST_ASSERT_TRUE(significantChange(state, next));
    state = high;
    state.meter.forward_power_watts = 990;
    TEST_ASSERT_TRUE(significantChange(high, state));
}

void test_active_interval_accumulation_and_latest_snapshot() {
    MqttStateSchedule schedule;
    const auto previous = measured();
    schedule.published(100, previous);
    auto state = previous;
    state.meter.forward_power_watts = 100.4;
    TEST_ASSERT_FALSE(schedule.due(300, state));
    state.meter.forward_power_watts = 100.8;
    TEST_ASSERT_FALSE(schedule.due(600, state));
    state.meter.forward_power_watts = 101.2;
    TEST_ASSERT_FALSE(schedule.due(1099, state));
    state.meter.forward_power_watts = 105;
    TEST_ASSERT_TRUE(schedule.due(1100, state));
    schedule.published(1100, state);
    TEST_ASSERT_FALSE(schedule.due(2100, state));
    TEST_ASSERT_TRUE(schedule.due(31100, state));
}

void test_short_changes_are_latched_until_next_send() {
    MqttStateSchedule schedule;
    const auto previous = measured();
    schedule.published(0, previous);
    auto changed = previous;
    changed.mode = MODE_MANUAL;
    TEST_ASSERT_FALSE(schedule.due(100, changed));
    TEST_ASSERT_TRUE(schedule.due(1000, previous));
    schedule.published(1000, previous);
    TEST_ASSERT_FALSE(schedule.due(2000, previous));
}

void test_zero_transitions_and_non_numeric_states() {
    auto previous = measured(0.001, 0.001);
    auto state = previous;
    state.meter.forward_power_watts = 0;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    TEST_ASSERT_TRUE(significantChange(state, previous));
    state = previous;
    state.meter.reflected_power_watts = 0;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    TEST_ASSERT_TRUE(significantChange(state, previous));
    state = previous;
    state.meter.swr = NAN;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    auto invalid = state;
    invalid.meter.swr = INFINITY;
    TEST_ASSERT_FALSE(significantChange(state, invalid));
    TEST_ASSERT_TRUE(significantChange(invalid, previous));
    state = previous;
    state.meterValid = false;
    TEST_ASSERT_TRUE(significantChange(previous, state));
}

void test_discrete_state_changes() {
    const auto previous = measured();
    auto state = previous;
    state.meter.band = BAND_40M;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    state = previous;
    state.mode = MODE_UNKNOWN;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    state = previous;
    state.antenna = ANT_B;
    TEST_ASSERT_TRUE(significantChange(previous, state));
    state = previous;
    state.remote = true;
    TEST_ASSERT_TRUE(significantChange(previous, state));
}

void test_failed_send_immediate_requests_and_millis_wrap() {
    MqttStateSchedule schedule;
    auto state = measured();
    schedule.published(UINT32_MAX - 499, state);
    state.mode = MODE_MANUAL;
    TEST_ASSERT_FALSE(schedule.due(499, state));
    TEST_ASSERT_TRUE(schedule.due(500, state));
    // A failed write does not call published(): the change is still pending.
    TEST_ASSERT_TRUE(schedule.due(501, state));
    schedule.published(501, state);
    TEST_ASSERT_FALSE(schedule.due(30500, state));
    TEST_ASSERT_TRUE(schedule.due(30501, state));
    schedule.published(30501, state);
    schedule.requestImmediate();
    TEST_ASSERT_TRUE(schedule.due(30502, state));
    TEST_ASSERT_TRUE(schedule.due(30503, state));
    schedule.published(30503, state);
    TEST_ASSERT_FALSE(schedule.due(30504, state));
}

void test_identities_and_all_discovery_payloads() {
    const auto first = mqttIdentity("AA:BB:CC:01:02:03");
    const auto second = mqttIdentity("AA:BB:CC:01:02:04");
    TEST_ASSERT_EQUAL_STRING("ldg/tuner/aabbcc010203", first.base.c_str());
    TEST_ASSERT_EQUAL_STRING("ldg_aabbcc010203", first.node.c_str());
    TEST_ASSERT_EQUAL_STRING(first.base.c_str(), mqttIdentity("aabbcc010203").base.c_str());
    TEST_ASSERT_TRUE(mqttIdentity("invalid").base.empty());
    std::set<std::string> ids, topics;
    for (const auto& identity : {first, second}) {
        for (size_t i = 0; i < MQTT_ENTITY_COUNT; ++i) {
            JsonDocument doc;
            writeDiscovery(doc, identity, i, "ESP32-3248S035C", "https://255.255.255.255/");
            std::string payload;
            serializeJson(doc, payload);
            const auto topic = discoveryTopic(identity, i);
            TEST_ASSERT_TRUE(mqttPacketFits(topic, payload.size()));
            TEST_ASSERT_TRUE(ids.insert(doc["unique_id"].as<std::string>()).second);
            TEST_ASSERT_TRUE(topics.insert(topic).second);
            TEST_ASSERT_EQUAL_STRING(identity.node.c_str(), doc["device"]["identifiers"][0]);
            TEST_ASSERT_EQUAL_STRING((identity.base + "/status").c_str(), doc["availability_topic"]);
            TEST_ASSERT_EQUAL_STRING("https://255.255.255.255/", doc["device"]["configuration_url"]);
            TEST_ASSERT_EQUAL_STRING("Remote-LDGControl", doc["origin"]["name"]);
            if (i < 6) {
                TEST_ASSERT_EQUAL_STRING((identity.base + "/state").c_str(), doc["state_topic"]);
                TEST_ASSERT_TRUE(doc["value_template"].is<const char*>());
                if (i < 2) {
                    TEST_ASSERT_EQUAL_STRING("power", doc["device_class"]);
                    TEST_ASSERT_EQUAL_STRING("W", doc["unit_of_measurement"]);
                    TEST_ASSERT_EQUAL_STRING("measurement", doc["state_class"]);
                }
            } else {
                TEST_ASSERT_EQUAL_STRING((identity.base + "/command").c_str(), doc["command_topic"]);
                TEST_ASSERT_FALSE(doc["retain"].as<bool>());
                TEST_ASSERT_EQUAL_INT(0, doc["qos"].as<int>());
                std::string command = doc["payload_press"].as<std::string>();
                TEST_ASSERT_NULL(parseCommand(reinterpret_cast<const uint8_t*>(command.data()), command.size()).error);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(24, ids.size());
    const std::string topic = first.base + "/state";
    TEST_ASSERT_TRUE(mqttPacketFits(topic, MQTT_PACKET_SIZE - 7 - topic.size()));
    TEST_ASSERT_FALSE(mqttPacketFits(topic, MQTT_PACKET_SIZE - 6 - topic.size()));
}

void test_state_json_unknown_values_and_web_round_trip() {
    JsonDocument doc;
    writeState(doc, tuner_state_t{});
    TEST_ASSERT_TRUE(doc["fwd_power"].isNull());
    TEST_ASSERT_TRUE(doc["swr"].isNull());
    TEST_ASSERT_EQUAL_STRING("unknown", doc["mode"]);
    TEST_ASSERT_EQUAL_STRING("unknown", doc["antenna"]);
    auto state = measured(0, 0, INFINITY);
    state.remote = true;
    doc.clear();
    writeState(doc, state);
    std::string payload;
    serializeJson(doc, payload);
    TEST_ASSERT_FALSE(deserializeJson(doc, payload));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, doc["fwd_power"].as<double>());
    TEST_ASSERT_TRUE(doc["swr"].isNull());
    TEST_ASSERT_EQUAL_STRING("remote", doc["source"]);
    TEST_ASSERT_EQUAL_STRING("auto", doc["mode"]);
    doc.clear();
    state.meter.swr = 1.0;
    writeState(doc, state, false);
    TEST_ASSERT_EQUAL_INT(MODE_AUTO, doc["mode"].as<int>());
    TEST_ASSERT_EQUAL_INT(ANT_A, doc["antenna"].as<int>());
    auto restored = readWebState(doc.as<JsonVariantConst>());
    restored.remote = true;
    TEST_ASSERT_FALSE(significantChange(state, restored));
    TEST_ASSERT_TRUE(restored.meterValid);
    doc.clear();
    deserializeJson(doc, "{\"fwd_power\":null,\"ref_power\":null,\"mode\":99,\"antenna\":99}");
    restored = readWebState(doc.as<JsonVariantConst>());
    TEST_ASSERT_FALSE(restored.meterValid);
    TEST_ASSERT_EQUAL_INT(MODE_UNKNOWN, restored.mode);
    TEST_ASSERT_EQUAL_INT(ANT_UNKNOWN, restored.antenna);
}

void test_active_source_keeps_remote_band_mode_and_antenna() {
    ActiveTunerState active;
    const auto local = measured();
    TEST_ASSERT_FALSE(active.select(local).remote);
    JsonDocument doc;
    deserializeJson(doc, "{\"fwd_power\":250,\"ref_power\":10,\"swr\":1.5,\"band\":\"40m\",\"mode\":2,\"antenna\":2}");
    active.updateRemote(readWebState(doc.as<JsonVariantConst>()));
    auto state = active.select(local);
    TEST_ASSERT_TRUE(active.remoteConnected());
    TEST_ASSERT_TRUE(state.remote);
    TEST_ASSERT_EQUAL_INT(BAND_40M, state.meter.band);
    TEST_ASSERT_EQUAL_INT(MODE_MANUAL, state.mode);
    TEST_ASSERT_EQUAL_INT(ANT_B, state.antenna);
    TEST_ASSERT_EQUAL_DOUBLE(250, state.meter.forward_power_watts);
    active.disconnectRemote();
    state = active.select(local);
    TEST_ASSERT_FALSE(active.remoteConnected());
    TEST_ASSERT_FALSE(state.remote);
    TEST_ASSERT_EQUAL_INT(MODE_AUTO, state.mode);
    TEST_ASSERT_EQUAL_INT(ANT_A, state.antenna);
}

struct Message {
    std::string topic, payload;
    bool retained;
};

class TestBroker : public MqttTransport {
public:
    std::vector<Message> messages;
    std::vector<std::string> subscriptions;
    std::map<std::string, std::string> retained;
    bool failPublish = false;
    bool failSubscribe = false;
    std::string oversizedTopic;
    void packetTooLarge(const char* topic) override { oversizedTopic = topic; }
    bool publish(const char* topic, const char* payload, bool retain) override {
        if (failPublish) return false;
        messages.push_back({topic, payload, retain});
        if (retain) {
            if (*payload) retained[topic] = payload;
            else retained.erase(topic);
        }
        return true;
    }
    bool subscribe(const char* topic) override {
        if (failSubscribe) return false;
        subscriptions.push_back(topic);
        return true;
    }
};

static unsigned executedCommands;
static uint8_t lastCommand;
static bool commandResult;
static uint32_t testClock;
static uint32_t readTestClock() { return testClock; }
static bool execute(uint8_t command) {
    ++executedCommands;
    lastCommand = command;
    return commandResult;
}
static void settle(MqttSession& session, uint32_t now, const tuner_state_t& state) {
    for (size_t i = 0; i < MQTT_ENTITY_COUNT; ++i) session.loop(now, state);
}
static void deliver(MqttSession& session, const std::string& topic, const std::string& payload) {
    session.message(topic.c_str(), reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

void test_oversized_session_packet_reports_topic_and_retries() {
    TestBroker broker;
    const auto id = mqttIdentity("aa:bb:cc:01:02:03");
    MqttSession session(broker, id, execute);
    const std::string hugeModel(MQTT_PACKET_SIZE, 'X');
    TEST_ASSERT_TRUE(session.connected(true, hugeModel.c_str(), "http://192.0.2.1/"));
    const auto count = broker.messages.size();
    session.loop(0, measured());
    TEST_ASSERT_EQUAL_UINT(count, broker.messages.size());
    TEST_ASSERT_EQUAL_STRING(discoveryTopic(id, 0).c_str(), broker.oversizedTopic.c_str());
    TEST_ASSERT_TRUE(session.connected(true, "ESP32", "http://192.0.2.1/"));
    session.loop(0, measured());
    TEST_ASSERT_EQUAL_STRING(discoveryTopic(id, 0).c_str(), broker.messages.back().topic.c_str());
}

void test_session_times_interval_from_completed_write() {
    TestBroker broker;
    const auto id = mqttIdentity("aa:bb:cc:01:02:03");
    MqttSession session(broker, id, execute, readTestClock);
    TEST_ASSERT_TRUE(session.connected(true, "ESP32", "https://192.0.2.1/"));
    testClock = 100;
    settle(session, 0, measured());
    const size_t count = broker.messages.size();
    const auto changed = measured(110);
    session.loop(1099, changed);
    TEST_ASSERT_EQUAL_UINT(count, broker.messages.size());
    testClock = 1200;
    session.loop(1100, changed);
    TEST_ASSERT_EQUAL_UINT(count + 1, broker.messages.size());
    session.loop(2199, measured(120));
    TEST_ASSERT_EQUAL_UINT(count + 1, broker.messages.size());
    testClock = 2200;
    session.loop(2200, measured(120));
    TEST_ASSERT_EQUAL_UINT(count + 2, broker.messages.size());
}

void test_session_reconnect_ha_birth_and_discovery_cleanup() {
    TestBroker broker;
    const auto id = mqttIdentity("aa:bb:cc:01:02:03");
    MqttSession session(broker, id, execute);
    TEST_ASSERT_TRUE(session.connected(true, "ESP32 remote", "https://192.0.2.1/"));
    TEST_ASSERT_EQUAL_UINT(2, broker.subscriptions.size());
    TEST_ASSERT_EQUAL_STRING((id.base + "/command").c_str(), broker.subscriptions[0].c_str());
    TEST_ASSERT_EQUAL_STRING(HA_STATUS_TOPIC, broker.subscriptions[1].c_str());
    TEST_ASSERT_EQUAL_STRING("online", broker.retained[id.base + "/status"].c_str());
    const auto state = measured(0, 0, 1);
    settle(session, 0, state);
    TEST_ASSERT_EQUAL_UINT(13, broker.retained.size());
    TEST_ASSERT_EQUAL_STRING((id.base + "/state").c_str(), broker.messages.back().topic.c_str());
    TEST_ASSERT_FALSE(broker.messages.back().retained);
    size_t count = broker.messages.size();
    deliver(session, HA_STATUS_TOPIC, "offline");
    session.loop(1, state);
    TEST_ASSERT_EQUAL_UINT(count, broker.messages.size());
    deliver(session, HA_STATUS_TOPIC, "online");
    settle(session, 1, state);
    TEST_ASSERT_EQUAL_UINT(count + 13, broker.messages.size());
    // A restarted broker has lost retained configs; reconnect repopulates all.
    broker.retained.clear();
    TEST_ASSERT_TRUE(session.connected(true, "ESP32 remote", "https://192.0.2.2/"));
    settle(session, 2, state);
    TEST_ASSERT_EQUAL_UINT(13, broker.retained.size());
    JsonDocument doc;
    deserializeJson(doc, broker.retained[discoveryTopic(id, 0)]);
    TEST_ASSERT_EQUAL_STRING("https://192.0.2.2/", doc["device"]["configuration_url"]);
    session.disconnect();
    TEST_ASSERT_EQUAL_STRING("offline", broker.retained[id.base + "/status"].c_str());
    TEST_ASSERT_TRUE(session.connected(false, "ESP32 remote", "https://192.0.2.2/"));
    settle(session, 3, state);
    TEST_ASSERT_EQUAL_UINT(1, broker.retained.size());
    count = broker.messages.size();
    deliver(session, HA_STATUS_TOPIC, "online");
    session.loop(4, state);
    TEST_ASSERT_EQUAL_UINT(count, broker.messages.size());
}

void test_session_two_devices_commands_and_error_responses() {
    TestBroker broker;
    const auto first = mqttIdentity("aa:bb:cc:01:02:03");
    const auto second = mqttIdentity("aa:bb:cc:01:02:04");
    MqttSession a(broker, first, execute), b(broker, second, execute);
    TEST_ASSERT_TRUE(a.connected(true, "ESP32", "https://192.0.2.1/"));
    TEST_ASSERT_TRUE(b.connected(true, "ESP32", "https://192.0.2.2/"));
    settle(a, 0, measured());
    settle(b, 0, measured());
    TEST_ASSERT_EQUAL_UINT(26, broker.retained.size());
    executedCommands = 0;
    commandResult = true;
    const uint8_t bytes[] = {'T', 'F', 'P', 'A', 'C', 'M'};
    for (size_t i = 6; i < MQTT_ENTITY_COUNT; ++i) {
        std::string command = std::string("{\"command\":\"") + mqttEntity(i).command + "\"}";
        deliver(a, first.base + "/command", command);
        deliver(b, first.base + "/command", command);
        TEST_ASSERT_EQUAL_UINT(i - 5, executedCommands);
        TEST_ASSERT_EQUAL_UINT8(bytes[i - 6], lastCommand);
        TEST_ASSERT_EQUAL_STRING((first.base + "/command/response").c_str(), broker.messages.back().topic.c_str());
        TEST_ASSERT_FALSE(broker.messages.back().retained);
        JsonDocument response;
        deserializeJson(response, broker.messages.back().payload);
        TEST_ASSERT_EQUAL_STRING("success", response["result"]);
    }
    for (const char* legacy : {"ldg/tuner/command", "ldg/tuner/remote/command"})
        deliver(a, legacy, "{\"command\":\"full_tune\"}");
    TEST_ASSERT_EQUAL_UINT(6, executedCommands);
    const char* invalid[] = {"{", "{}", "{\"command\":65}", "{\"command\":\"bad\"}"};
    const char* errors[] = {"invalid_json", "missing_command", "missing_command", "unknown_command"};
    for (size_t i = 0; i < 4; ++i) {
        deliver(a, first.base + "/command", invalid[i]);
        JsonDocument response;
        deserializeJson(response, broker.messages.back().payload);
        TEST_ASSERT_EQUAL_STRING(errors[i], response["result"]);
    }
    TEST_ASSERT_EQUAL_UINT(6, executedCommands);
    commandResult = false;
    deliver(a, first.base + "/command", "{\"command\":\"manual\"}");
    JsonDocument response;
    deserializeJson(response, broker.messages.back().payload);
    TEST_ASSERT_EQUAL_STRING("failed", response["result"]);
    a.loop(1, measured());
    TEST_ASSERT_EQUAL_STRING((first.base + "/state").c_str(), broker.messages.back().topic.c_str());
}

void test_session_failed_publishes_retry_and_keep_newest_state() {
    TestBroker broker;
    const auto id = mqttIdentity("aa:bb:cc:01:02:03");
    MqttSession session(broker, id, execute);
    broker.failSubscribe = true;
    TEST_ASSERT_FALSE(session.connected(true, "ESP32", "https://192.0.2.1/"));
    broker.failSubscribe = false;
    TEST_ASSERT_TRUE(session.connected(true, "ESP32", "https://192.0.2.1/"));
    broker.failPublish = true;
    settle(session, 0, measured());
    TEST_ASSERT_EQUAL_UINT(1, broker.retained.size());
    broker.failPublish = false;
    settle(session, 0, measured());
    auto state = measured(101);
    session.loop(999, state);
    TEST_ASSERT_EQUAL_UINT(14, broker.messages.size());
    broker.failPublish = true;
    session.loop(1000, state);
    state.meter.forward_power_watts = 102;
    broker.failPublish = false;
    session.loop(1001, state);
    JsonDocument doc;
    deserializeJson(doc, broker.messages.back().payload);
    TEST_ASSERT_EQUAL_DOUBLE(102, doc["fwd_power"].as<double>());
    size_t count = broker.messages.size();
    session.loop(31000, state);
    TEST_ASSERT_EQUAL_UINT(count, broker.messages.size());
    session.loop(31001, state);
    TEST_ASSERT_EQUAL_UINT(count + 1, broker.messages.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_interval_including_zero);
    RUN_TEST(test_absolute_and_relative_threshold_boundaries);
    RUN_TEST(test_active_interval_accumulation_and_latest_snapshot);
    RUN_TEST(test_short_changes_are_latched_until_next_send);
    RUN_TEST(test_zero_transitions_and_non_numeric_states);
    RUN_TEST(test_discrete_state_changes);
    RUN_TEST(test_failed_send_immediate_requests_and_millis_wrap);
    RUN_TEST(test_identities_and_all_discovery_payloads);
    RUN_TEST(test_state_json_unknown_values_and_web_round_trip);
    RUN_TEST(test_active_source_keeps_remote_band_mode_and_antenna);
    RUN_TEST(test_oversized_session_packet_reports_topic_and_retries);
    RUN_TEST(test_session_times_interval_from_completed_write);
    RUN_TEST(test_session_reconnect_ha_birth_and_discovery_cleanup);
    RUN_TEST(test_session_two_devices_commands_and_error_responses);
    RUN_TEST(test_session_failed_publishes_retry_and_keep_newest_state);
    return UNITY_END();
}
