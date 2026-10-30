#pragma once
#include "WiFi.h"
#include <functional>
struct PubSubClient {
    static PubSubClient* instance;
    static bool bufferOk;
    bool online = false;
    bool connectOk = true;
    bool loopOk = true;
    bool publishOk = true;
    bool subscribeOk = true;
    int code = -1;
    int connectCode = -2;
    int loopCode = -4;
    unsigned connectCalls = 0;
    explicit PubSubClient(WiFiClient&) { instance = this; }
    void setServer(const char*, uint16_t) {}
    void setKeepAlive(int) {}
    void setSocketTimeout(int) {}
    bool setBufferSize(size_t) { return bufferOk; }
    void setCallback(std::function<void(char*, byte*, unsigned int)>) {}
    bool connect(const char*, const char*, const char*, const char*, int, bool, const char*, bool) {
        ++connectCalls;
        online = connectOk;
        code = connectOk ? 0 : connectCode;
        return online;
    }
    bool connected() {
        if (!online && code == 0) code = -3;
        return online;
    }
    bool loop() {
        if (!loopOk) { online = false; code = loopCode; }
        return loopOk;
    }
    void disconnect() { online = false; code = -1; }
    int state() const { return code; }
    bool publish(const char*, const char*, bool) { return online && publishOk; }
    bool subscribe(const char*, int) { return online && subscribeOk; }
};
