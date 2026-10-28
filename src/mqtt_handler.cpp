#include "mqtt_handler.h"
#include "config_manager.h"

MqttHandler::~MqttHandler() {
    delete m_session;
    delete m_client;
}

bool MqttHandler::begin(WiFiClient& wifiClient, mqtt_command_handler_t cmdHandler,
                        tuner_state_getter_t stateGetter) {
    const DeviceConfig& cfg = configManager.get();
    if (m_client || !cfg.mqttBroker[0]) return false;
    m_identity = ldg::mqttIdentity(WiFi.macAddress().c_str());
    if (m_identity.base.empty()) return false;

    wifiClient.setTimeout(1); // Arduino ESP32 uses seconds, including TCP connect timeout.
    m_client = new PubSubClient(wifiClient);
    m_client->setServer(cfg.mqttBroker, cfg.mqttPort);
    m_client->setKeepAlive(LDG_MQTT_KEEPALIVE);
    m_client->setSocketTimeout(2);
    if (!m_client->setBufferSize(ldg::MQTT_PACKET_SIZE)) {
        delete m_client;
        m_client = nullptr;
        return false;
    }
    m_stateGetter = stateGetter;
    m_session = new ldg::MqttSession(*this, m_identity, cmdHandler,
                                    []() -> uint32_t { return millis(); });
    m_client->setCallback([this](char* topic, byte* payload, unsigned int length) {
        m_session->message(topic, payload, length);
    });
    return true;
}

bool MqttHandler::connect() {
    const DeviceConfig& cfg = configManager.get();
    const std::string clientId = std::string(MQTT_CLIENT_PREFIX) + "-" + m_identity.mac;
    const std::string statusTopic = m_identity.base + "/status";
    const char* user = cfg.mqttUsername[0] ? cfg.mqttUsername : nullptr;
    const char* password = cfg.mqttUsername[0] ? cfg.mqttPassword : nullptr;
    if (!m_client->connect(clientId.c_str(), user, password,
                           statusTopic.c_str(), 0, true, "offline", true)) return false;

#ifdef BOARD_ESP32_3248S035C
    const char* model = "ESP32-3248S035C";
#elif defined(WITH_DISPLAY)
    const char* model = "ESP32 display controller";
#else
    const char* model = "ESP32 remote controller";
#endif
    const String url = String(WEB_SCHEME) + WiFi.localIP().toString() + "/";
    if (!m_session->connected(cfg.mqttDiscoveryEnabled, model, url.c_str())) {
        disconnect();
        return false;
    }
    Serial.printf("MQTT connected: %s\n", m_identity.base.c_str());
    return true;
}

bool MqttHandler::isConnected() const {
    return m_client && m_client->connected();
}

void MqttHandler::disconnect() {
    if (isConnected()) {
        m_session->disconnect();
        m_client->disconnect();
    }
}

void MqttHandler::loop() {
    if (!m_client || WiFi.status() != WL_CONNECTED) return;
    if (!isConnected()) {
        const uint32_t now = millis();
        if (now - m_lastReconnectAttempt < 5000) return;
        m_lastReconnectAttempt = now;
        if (!connect()) return;
    }
    if (!m_client->loop()) return;
    if (m_stateRequested.exchange(false)) m_session->requestImmediate();
    if (m_stateGetter) {
        const tuner_state_t state = m_stateGetter();
        m_session->loop(millis(), state);
    }
}

bool MqttHandler::publish(const char* topic, const char* payload, bool retained) {
    return m_client->publish(topic, payload, retained);
}

bool MqttHandler::subscribe(const char* topic) {
    return m_client->subscribe(topic, 0);
}
