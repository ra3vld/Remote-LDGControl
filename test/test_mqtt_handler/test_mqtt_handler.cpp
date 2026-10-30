#include <unity.h>
#include "../support/mqtt/config_manager.h"
#include "mqtt_handler.h"

// Exercise the production handler against a controllable network/transport.
// Native tests do not otherwise compile ESP32 sources.
#include "../../src/mqtt_handler.cpp"

uint32_t testMillis = 0;
FakeSerial Serial;
FakeWiFi WiFi;
FakeConfigManager configManager;
PubSubClient* PubSubClient::instance = nullptr;
bool PubSubClient::bufferOk = true;

void setUp() {
    testMillis = 5000;
    Serial.output.clear();
    WiFi = FakeWiFi{};
    configManager.config = DeviceConfig{};
    PubSubClient::bufferOk = true;
}
void tearDown() {}
static tuner_state_t state() { return {}; }

void test_disabled_and_initialization_error_are_distinct() {
    WiFiClient wifi;
    MqttHandler disabled;
    configManager.config.mqttBroker[0] = '\0';
    TEST_ASSERT_FALSE(disabled.begin(wifi, nullptr, state));
    TEST_ASSERT_TRUE(disabled.getStatus().state == ldg::MqttConnection::Disabled);
    configManager.config = DeviceConfig{};
    WiFi.mac = "invalid";
    MqttHandler invalid;
    TEST_ASSERT_FALSE(invalid.begin(wifi, nullptr, state));
    TEST_ASSERT_TRUE(invalid.getStatus().state == ldg::MqttConnection::Error);
    TEST_ASSERT_TRUE(invalid.getStatus().lastError == ldg::MqttError::Identity);
    WiFi = FakeWiFi{};
    PubSubClient::bufferOk = false;
    MqttHandler noMemory;
    TEST_ASSERT_FALSE(noMemory.begin(wifi, nullptr, state));
    TEST_ASSERT_TRUE(noMemory.getStatus().state == ldg::MqttConnection::Error);
    TEST_ASSERT_TRUE(noMemory.getStatus().lastError == ldg::MqttError::Memory);
}

void test_broker_failures_retry_and_recovery_preserves_error() {
    for (int code : {-4, -3, -2, -1, 1, 2, 3, 4, 5}) {
        WiFiClient wifi;
        MqttHandler mqtt;
        TEST_ASSERT_TRUE(mqtt.begin(wifi, nullptr, state));
        auto& client = *PubSubClient::instance;
        client.connectOk = false;
        client.connectCode = code;
        mqtt.loop();
        TEST_ASSERT_FALSE(mqtt.isConnected());
        TEST_ASSERT_EQUAL_INT(code, mqtt.getStatus().errorCode);
        TEST_ASSERT_TRUE(mqtt.getStatus().errorActive);
        testMillis += 4999;
        mqtt.loop();
        TEST_ASSERT_EQUAL_UINT(1, client.connectCalls);
        testMillis += 1;
        client.connectOk = true;
        mqtt.loop();
        TEST_ASSERT_TRUE(mqtt.isConnected());
        TEST_ASSERT_FALSE(mqtt.getStatus().errorActive);
        TEST_ASSERT_EQUAL_INT(code, mqtt.getStatus().errorCode);
        TEST_ASSERT_EQUAL_UINT(2, mqtt.getStatus().attempts);
        JsonDocument doc;
        ldg::writeMqttStatus(doc.to<JsonObject>(), mqtt.getStatus(), testMillis);
        TEST_ASSERT_EQUAL_INT(code, doc["lastError"]["code"].as<int>());
        TEST_ASSERT_EQUAL_STRING(ldg::mqttClientStateMessage(code), doc["lastError"]["detail"]);
    }
    TEST_ASSERT_TRUE(Serial.output.find("secret-must-not-appear") == std::string::npos);
}

void test_wifi_loss_and_keepalive_timeout() {
    WiFiClient wifi;
    MqttHandler mqtt;
    WiFi.wifiStatus = 0;
    TEST_ASSERT_TRUE(mqtt.begin(wifi, nullptr, state));
    mqtt.loop();
    TEST_ASSERT_EQUAL_UINT(0, PubSubClient::instance->connectCalls);
    TEST_ASSERT_TRUE(mqtt.getStatus().state == ldg::MqttConnection::WaitingWifi);
    WiFi.wifiStatus = WL_CONNECTED;
    mqtt.loop();
    TEST_ASSERT_TRUE(mqtt.isConnected());
    WiFi.wifiStatus = 0;
    mqtt.loop();
    TEST_ASSERT_FALSE(mqtt.isConnected());
    TEST_ASSERT_TRUE(mqtt.getStatus().lastError == ldg::MqttError::WifiLost);
    const auto log = Serial.output;
    for (int i = 0; i < 10; ++i) mqtt.loop();
    TEST_ASSERT_EQUAL_STRING(log.c_str(), Serial.output.c_str());
    testMillis += 5000;
    WiFi.wifiStatus = WL_CONNECTED;
    mqtt.loop();
    TEST_ASSERT_TRUE(mqtt.isConnected());
    PubSubClient::instance->loopOk = false;
    mqtt.loop();
    TEST_ASSERT_FALSE(mqtt.isConnected());
    TEST_ASSERT_EQUAL_INT(-4, mqtt.getStatus().errorCode);
    TEST_ASSERT_TRUE(mqtt.getStatus().lastError == ldg::MqttError::ConnectionLost);
}

void test_session_setup_and_publish_failures_keep_their_topic_and_do_not_spam() {
    WiFiClient wifi;
    MqttHandler mqtt;
    TEST_ASSERT_TRUE(mqtt.begin(wifi, nullptr, state));
    auto& client = *PubSubClient::instance;
    client.subscribeOk = false;
    mqtt.loop();
    TEST_ASSERT_FALSE(mqtt.isConnected());
    TEST_ASSERT_TRUE(mqtt.getStatus().lastError == ldg::MqttError::Subscribe);
    TEST_ASSERT_EQUAL_STRING("ldg/tuner/aabbcc010203/command", mqtt.getStatus().errorTopic);
    testMillis += 5000;
    client.subscribeOk = true;
    client.publishOk = false;
    mqtt.loop();
    TEST_ASSERT_FALSE(mqtt.isConnected());
    TEST_ASSERT_TRUE(mqtt.getStatus().lastError == ldg::MqttError::Publish);
    TEST_ASSERT_EQUAL_STRING("ldg/tuner/aabbcc010203/status", mqtt.getStatus().errorTopic);
    testMillis += 5000;
    client.publishOk = true;
    mqtt.loop();
    TEST_ASSERT_TRUE(mqtt.isConnected());
    client.publishOk = false;
    mqtt.loop();
    TEST_ASSERT_TRUE(mqtt.isConnected()); // Transport may still be connected after a short write.
    TEST_ASSERT_TRUE(mqtt.getStatus().errorActive);
    const auto log = Serial.output;
    for (int i = 0; i < 10; ++i) { ++testMillis; mqtt.loop(); }
    TEST_ASSERT_EQUAL_STRING(log.c_str(), Serial.output.c_str());
    client.publishOk = true;
    mqtt.loop();
    TEST_ASSERT_FALSE(mqtt.getStatus().errorActive);
    TEST_ASSERT_TRUE(mqtt.getStatus().lastError == ldg::MqttError::Publish);
}

void test_status_retry_and_error_age_survive_millis_wrap() {
    ldg::MqttStatus status;
    status.state = ldg::MqttConnection::Disconnected;
    status.lastAttemptMs = UINT32_MAX - 999;
    status.failed(ldg::MqttError::Connect, 5, nullptr, UINT32_MAX - 499);
    JsonDocument doc;
    ldg::writeMqttStatus(doc.to<JsonObject>(), status, 1000);
    TEST_ASSERT_EQUAL_UINT(3000, doc["retryInMs"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT(1500, doc["lastError"]["ageMs"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["lastError"]["topic"].isNull());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_and_initialization_error_are_distinct);
    RUN_TEST(test_broker_failures_retry_and_recovery_preserves_error);
    RUN_TEST(test_wifi_loss_and_keepalive_timeout);
    RUN_TEST(test_session_setup_and_publish_failures_keep_their_topic_and_do_not_spam);
    RUN_TEST(test_status_retry_and_error_age_survive_millis_wrap);
    return UNITY_END();
}
