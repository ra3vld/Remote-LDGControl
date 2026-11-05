#include "tci_client.h"
#include "config_manager.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>

void TciClient::begin() {
    m_socket.onEvent([this](WStype_t type, uint8_t* payload, size_t length) { event(type, payload, length); });
    m_socket.setReconnectInterval(2000);
    m_socket.enableHeartbeat(1000, 2000, 1);
    publish();
}

void TciClient::publish() {
    auto snapshot = m_protocol.snapshot(millis());
    snapshot.provider = m_applied.enabled ? ldg::RadioProvider::Tci : ldg::RadioProvider::None;
    if (millis() - m_metricsAt >= 1000) {
        m_metricsAt = millis();
        snapshot.freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snapshot.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snapshot.stackFree = uxTaskGetStackHighWaterMark(nullptr);
    } else {
        snapshot.freeHeap = m_snapshot.freeHeap;
        snapshot.largestBlock = m_snapshot.largestBlock;
        snapshot.stackFree = m_snapshot.stackFree;
    }
    portENTER_CRITICAL(&m_mux);
    m_snapshot = snapshot;
    m_publishedAt = millis();
    portEXIT_CRITICAL(&m_mux);
}

ldg::RadioState TciClient::state() const {
    portENTER_CRITICAL(&m_mux);
    auto snapshot = m_snapshot;
    const uint32_t elapsed = millis() - m_publishedAt;
    const auto applied = m_applied;
    portEXIT_CRITICAL(&m_mux);
    snapshot.ageMs = snapshot.ageMs > UINT32_MAX - elapsed ? UINT32_MAX : snapshot.ageMs + elapsed;
    snapshot.stableMs = snapshot.stableMs > UINT32_MAX - elapsed ? UINT32_MAX : snapshot.stableMs + elapsed;
    // Saving a new endpoint immediately revokes the old endpoint's authority,
    // even when its network task is still inside DNS/connect/read.
    const auto configured = configManager.radioSettings();
    if (!ldg::sameRadioSettings(configured, applied)) {
        snapshot = ldg::RadioState{};
        snapshot.provider = configured.enabled ? ldg::RadioProvider::Tci : ldg::RadioProvider::None;
        snapshot.receiver = configured.receiver;
        snapshot.connection = configured.enabled ? ldg::RadioConnection::Connecting : ldg::RadioConnection::Disabled;
    } else if (snapshot.ownsBand() && snapshot.ageMs > ldg::RadioState::LivenessMs) {
        m_expired.store(true);
        snapshot.connection = ldg::RadioConnection::Disconnected;
        snapshot.frequencyValid = false;
        strcpy(snapshot.error, "TCI heartbeat timeout");
    }
    return snapshot;
}

void TciClient::event(WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
        m_expired.store(false);
        m_connectedAt = millis(); m_lastQueries = m_connectedAt;
        m_textFragment = false;
        m_protocol.connect(m_applied.receiver, millis());
        Serial.println("[TCI] Connected; synchronizing");
    } else if (type == WStype_DISCONNECTED || type == WStype_ERROR) {
        m_textFragment = false;
        m_protocol.disconnect(m_applied.enabled, "TCI disconnected; using LDG telemetry");
    } else if (m_expired.load() || m_protocol.snapshot(millis()).connection == ldg::RadioConnection::Disconnected) {
        m_resetRequested = true; // A late pong cannot revive an expired session.
    } else if (type == WStype_PONG || type == WStype_PING) m_protocol.heartbeat(millis());
    else if (type == WStype_TEXT || type == WStype_FRAGMENT_TEXT_START ||
             ((type == WStype_FRAGMENT || type == WStype_FRAGMENT_FIN) && m_textFragment)) {
        if (type == WStype_FRAGMENT_TEXT_START) m_textFragment = true;
        if (!m_protocol.feed(payload, length, millis())) m_resetRequested = true;
        if (type == WStype_FRAGMENT_FIN) m_textFragment = false;
    } else if (type == WStype_FRAGMENT_BIN_START) m_textFragment = false;
    // Audio/IQ are never subscribed to and binary payloads are never parsed as CAT.
}

void TciClient::queryState() {
    const auto state = m_protocol.snapshot(millis());
    char query[96];
    snprintf(query, sizeof(query), "vfo:%u,0;vfo:%u,1;split_enable:%u;xit_enable:%u;xit_offset:%u;modulation:%u;",
             state.receiver, state.receiver, state.receiver, state.receiver, state.receiver, state.receiver);
    m_socket.sendTXT(query);
    for (unsigned i = 0; i < state.receiverCount; ++i) {
        snprintf(query, sizeof(query), "trx:%u;tune:%u;", i, i);
        m_socket.sendTXT(query);
    }
    m_lastQueries = millis();
}

void TciClient::loop(bool localOwner) {
    const auto configured = configManager.radioSettings();
    if (!ldg::sameRadioSettings(configured, m_applied) || localOwner != m_localOwner) {
        m_socket.disconnect();
        portENTER_CRITICAL(&m_mux);
        m_applied = configured;
        portEXIT_CRITICAL(&m_mux);
        m_localOwner = localOwner;
        m_started = false;
        m_protocol.disconnect(configured.enabled, localOwner ? "" : "Configure TCI on the tuner unit");
    }
    if (!configured.enabled || !localOwner || WiFi.status() != WL_CONNECTED) {
        if (m_started) m_socket.disconnect();
        m_started = false;
        m_protocol.disconnect(configured.enabled, !localOwner ? "Configure TCI on the tuner unit" :
                              configured.enabled ? "Waiting for Wi-Fi; using LDG telemetry" : "");
        publish(); return;
    }
    if (!m_started) {
        m_protocol.connecting(); publish();
        m_socket.begin(configured.host, configured.port, "/", "");
        m_started = true;
    }
    if (m_expired.exchange(false) || (m_socket.isConnected() &&
        m_protocol.snapshot(millis()).connection == ldg::RadioConnection::Disconnected)) {
        m_socket.disconnect();
        m_protocol.disconnect(true, "TCI heartbeat timeout; reconnecting");
    }
    m_socket.loop();
    const auto state = m_protocol.snapshot(millis());
    if (m_socket.isConnected()) {
        if (state.connection == ldg::RadioConnection::Ready) m_connectedAt = millis();
        if (m_protocol.initialized() && state.connection != ldg::RadioConnection::Ready &&
            millis() - m_lastQueries >= 1000) queryState();
        // An apparently open socket with incomplete state cannot live forever.
        if ((state.connection != ldg::RadioConnection::Ready && millis() - m_connectedAt > 10000) ||
            state.connection == ldg::RadioConnection::Disconnected) m_resetRequested = true;
    }
    if (m_resetRequested) {
        m_resetRequested = false;
        m_socket.disconnect();
        m_protocol.disconnect(true, "TCI state/heartbeat timeout; reconnecting");
    }
    publish();
}
