#pragma once
#include "WiFi.h"
class NetworkClientSecure : public WiFiClient {
public:
    static unsigned created, destroyed, bundles, handshakeTimeout;
    static std::string certificate;
    NetworkClientSecure() { ++created; }
    ~NetworkClientSecure() { ++destroyed; }
    void useBuiltinCACertBundle() { ++bundles; }
    void setCACert(const char* value) { certificate=value; }
    void setHandshakeTimeout(unsigned seconds) { handshakeTimeout=seconds; }
};
inline void configTime(long, int, const char*, const char*) {}
