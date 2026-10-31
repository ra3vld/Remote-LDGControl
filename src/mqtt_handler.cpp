#include "mqtt_handler.h"
#include "config_manager.h"
#include <new>

ldg::MqttStatus MqttHandler::getStatus() const {
    portENTER_CRITICAL(&m_statusMux);
    const ldg::MqttStatus status = m_status;
    portEXIT_CRITICAL(&m_statusMux);
    return status;
}

void MqttHandler::setConnectionState(ldg::MqttConnection state) {
    portENTER_CRITICAL(&m_statusMux);
    const bool changed = m_status.state != state;
    m_status.state = state;
    portEXIT_CRITICAL(&m_statusMux);
    if (changed) Serial.printf("[MQTT] %s\n", ldg::mqttConnectionMessage(state));
}

void MqttHandler::reportError(ldg::MqttError error, int code, const char* topic) {
    portENTER_CRITICAL(&m_statusMux);
    const bool changed = m_status.failed(error, code, topic, millis());
    portEXIT_CRITICAL(&m_statusMux);
    if (!changed) return; // A failing send is retried every loop; log only changes.
    Serial.printf("[MQTT] ERROR: %s", ldg::mqttErrorMessage(error));
    if (error == ldg::MqttError::Connect || error == ldg::MqttError::ConnectionLost ||
        error == ldg::MqttError::Publish || error == ldg::MqttError::Subscribe) {
        Serial.printf(" (code %d", code);
        if (code != 0) Serial.printf(": %s", ldg::mqttClientStateMessage(code));
        Serial.print(")");
    }
    if (topic && topic[0]) Serial.printf("; topic=%s", topic);
    Serial.println();
}

void MqttHandler::recoverOperation(ldg::MqttError operation, const char* topic) {
    portENTER_CRITICAL(&m_statusMux);
    const bool recovered = m_status.recover(operation, topic);
    portEXIT_CRITICAL(&m_statusMux);
    if (recovered) Serial.printf("[MQTT] Transport write recovered; topic=%s\n", topic);
}

MqttHandler::~MqttHandler() {
    delete m_session;
    delete m_client;
}

bool MqttHandler::begin(WiFiClient& wifiClient, mqtt_command_handler_t cmdHandler,
                        tuner_state_getter_t stateGetter) {
    if (m_wifiClient) return false;
    m_wifiClient = &wifiClient;
    m_commandHandler = cmdHandler;
    m_stateGetter = stateGetter;
    return configure();
}

bool MqttHandler::configure() {
    if (m_client) disconnect();
    delete m_session; m_session = nullptr;
    delete m_client; m_client = nullptr;
    portENTER_CRITICAL(&m_statusMux);
    m_status = ldg::MqttStatus{};
    m_status.lastAttemptMs = millis() - ldg::MQTT_RECONNECT_MS;
    portEXIT_CRITICAL(&m_statusMux);
    const DeviceConfig& cfg = configManager.get();
    if (!cfg.mqttBroker[0]) {
        Serial.printf("[MQTT] %s\n", ldg::mqttConnectionMessage(ldg::MqttConnection::Disabled));
        return false;
    }
    m_identity = ldg::mqttIdentity(WiFi.macAddress().c_str());
    if (m_identity.base.empty()) {
        setConnectionState(ldg::MqttConnection::Error);
        reportError(ldg::MqttError::Identity);
        return false;
    }

    m_wifiClient->setTimeout(1); // Arduino ESP32 uses seconds, including TCP connect timeout.
    m_client = new (std::nothrow) PubSubClient(*m_wifiClient);
    if (!m_client) {
        setConnectionState(ldg::MqttConnection::Error);
        reportError(ldg::MqttError::Memory);
        return false;
    }
    m_broker = cfg.mqttBroker;
    m_client->setServer(m_broker.c_str(), cfg.mqttPort);
    m_client->setKeepAlive(LDG_MQTT_KEEPALIVE);
    m_client->setSocketTimeout(2);
    if (!m_client->setBufferSize(ldg::MQTT_PACKET_SIZE)) {
        delete m_client;
        m_client = nullptr;
        setConnectionState(ldg::MqttConnection::Error);
        reportError(ldg::MqttError::Memory);
        return false;
    }
    m_session = new (std::nothrow) ldg::MqttSession(*this, m_identity, m_commandHandler,
                                    []() -> uint32_t { return millis(); }, &m_readings);
    if (!m_session) {
        delete m_client;
        m_client = nullptr;
        setConnectionState(ldg::MqttConnection::Error);
        reportError(ldg::MqttError::Memory);
        return false;
    }
    m_client->setCallback([this](char* topic, byte* payload, unsigned int length) {
        m_session->message(topic, payload, length);
    });
    Serial.printf("[MQTT] Broker=%s:%u; client=%s-%s; topic=%s; discovery=%s\n",
                  cfg.mqttBroker, cfg.mqttPort, MQTT_CLIENT_PREFIX, m_identity.mac.c_str(),
                  m_identity.base.c_str(), cfg.mqttDiscoveryEnabled ? "on" : "off");
    setConnectionState(WiFi.status() == WL_CONNECTED ? ldg::MqttConnection::Disconnected
                                                   : ldg::MqttConnection::WaitingWifi);
    return true;
}

bool MqttHandler::connect() {
    const DeviceConfig& cfg = configManager.get();
    const std::string clientId = std::string(MQTT_CLIENT_PREFIX) + "-" + m_identity.mac;
    const std::string statusTopic = m_identity.base + "/status";
    const char* user = cfg.mqttUsername[0] ? cfg.mqttUsername : nullptr;
    const char* password = cfg.mqttUsername[0] ? cfg.mqttPassword : nullptr;
    portENTER_CRITICAL(&m_statusMux);
    m_status.state = ldg::MqttConnection::Connecting;
    m_status.lastAttemptMs = millis();
    const uint32_t attempt = ++m_status.attempts;
    portEXIT_CRITICAL(&m_statusMux);
    Serial.printf("[MQTT] Connecting to %s:%u (attempt %lu)...\n",
                  cfg.mqttBroker, cfg.mqttPort, (unsigned long)attempt);
    if (!m_client->connect(clientId.c_str(), user, password,
                           statusTopic.c_str(), 0, true, "offline", true)) {
        reportError(ldg::MqttError::Connect, m_client->state());
        setConnectionState(ldg::MqttConnection::Disconnected);
        return false;
    }

#ifdef BOARD_ESP32_3248S035C
    const char* model = "ESP32-3248S035C";
#elif defined(WITH_DISPLAY)
    const char* model = "ESP32 display controller";
#else
    const char* model = "ESP32 remote controller";
#endif
    const String url = String(WEB_SCHEME) + WiFi.localIP().toString() + "/";
    if (!m_session->connected(cfg.mqttDiscoveryEnabled, model, url.c_str())) {
        // Keep the specific subscribe/publish error captured by the transport.
        m_client->disconnect();
        setConnectionState(ldg::MqttConnection::Disconnected);
        return false;
    }
    portENTER_CRITICAL(&m_statusMux);
    m_status.connected(millis());
    portEXIT_CRITICAL(&m_statusMux);
    Serial.printf("[MQTT] Connected to %s:%u; topic=%s\n",
                  cfg.mqttBroker, cfg.mqttPort, m_identity.base.c_str());
    return true;
}

bool MqttHandler::isConnected() const {
    return getStatus().state == ldg::MqttConnection::Connected;
}

void MqttHandler::disconnect() {
    if (m_client && m_client->connected()) {
        m_session->disconnect();
        m_client->disconnect();
    }
    if (m_client) setConnectionState(ldg::MqttConnection::Disconnected);
}

void MqttHandler::loop() {
    if (m_reconfigureRequested.exchange(false) && m_wifiClient) configure();
    // Observe RF even while the broker is offline. The history also survives
    // client/session reconstruction requested by the reconnect button.
    if (m_stateGetter) m_readings.observe(m_stateGetter());
    if (!m_client) return;
    if (WiFi.status() != WL_CONNECTED) {
        if (isConnected()) {
            reportError(ldg::MqttError::WifiLost);
            m_client->disconnect();
        }
        setConnectionState(ldg::MqttConnection::WaitingWifi);
        return;
    }
    if (!m_client->connected()) {
        const uint32_t now = millis();
        if (isConnected()) {
            reportError(ldg::MqttError::ConnectionLost, m_client->state());
            portENTER_CRITICAL(&m_statusMux);
            m_status.lastAttemptMs = now;
            portEXIT_CRITICAL(&m_statusMux);
        }
        setConnectionState(ldg::MqttConnection::Disconnected);
        if (now - getStatus().lastAttemptMs < ldg::MQTT_RECONNECT_MS) return;
        if (!connect()) return;
    }
    if (!m_client->loop()) {
        reportError(ldg::MqttError::ConnectionLost, m_client->state());
        portENTER_CRITICAL(&m_statusMux);
        m_status.lastAttemptMs = millis();
        portEXIT_CRITICAL(&m_statusMux);
        setConnectionState(ldg::MqttConnection::Disconnected);
        return;
    }
    if (m_stateRequested.exchange(false)) m_session->requestImmediate();
    if (m_stateGetter) {
        const tuner_state_t state = m_stateGetter();
        m_session->loop(millis(), state);
    }
}

bool MqttHandler::publish(const char* topic, const char* payload, bool retained) {
    const bool sent = m_client->publish(topic, payload, retained);
    if (!sent) reportError(ldg::MqttError::Publish, m_client->state(), topic);
    else {
        recoverOperation(ldg::MqttError::Publish, topic);
        recoverOperation(ldg::MqttError::PacketSize, topic);
    }
    return sent;
}

void MqttHandler::packetTooLarge(const char* topic) {
    reportError(ldg::MqttError::PacketSize, 0, topic);
}

bool MqttHandler::subscribe(const char* topic) {
    const bool sent = m_client->subscribe(topic, 0);
    if (!sent) reportError(ldg::MqttError::Subscribe, m_client->state(), topic);
    else recoverOperation(ldg::MqttError::Subscribe, topic);
    return sent;
}
