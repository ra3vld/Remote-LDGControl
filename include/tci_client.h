#pragma once

#include <Arduino.h>
#include <atomic>
#include <WebSocketsClient.h>
#include "tci_protocol.h"
#include "radio_settings.h"

class TciClient {
public:
    void begin();
    void loop(bool localOwner = true);
    ldg::RadioState state() const;
private:
    WebSocketsClient m_socket;
    ldg::TciProtocol m_protocol;
    ldg::RadioSettings m_applied;
    bool m_started = false, m_localOwner = true, m_textFragment = false;
    bool m_resetRequested = false;
    bool m_synchronized = false;
    char m_resetReason[64]{};
    mutable std::atomic<bool> m_expired{false};
    uint32_t m_connectedAt = 0, m_lastQueries = 0, m_metricsAt = 0;
    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
    ldg::RadioState m_snapshot;
    uint32_t m_publishedAt = 0;
    void event(WStype_t type, uint8_t* payload, size_t length);
    void publish();
    void queryState();
    void requestReset(const char* reason);
};
