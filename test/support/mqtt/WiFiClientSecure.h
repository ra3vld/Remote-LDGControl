#pragma once
#include "WiFi.h"
class NetworkClientSecure : public WiFiClient {
public:
    static unsigned created, destroyed, bundles;
    NetworkClientSecure() { ++created; }
    ~NetworkClientSecure() { ++destroyed; }
    void useBuiltinCACertBundle() { ++bundles; }
    void setHandshakeTimeout(unsigned) {}
};
inline void configTime(long, int, const char*, const char*) {}
