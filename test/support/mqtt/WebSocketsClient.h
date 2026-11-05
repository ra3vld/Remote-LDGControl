#pragma once
#include "Arduino.h"
#include <functional>
#include <vector>
enum WStype_t { WStype_ERROR, WStype_DISCONNECTED, WStype_CONNECTED, WStype_TEXT, WStype_BIN,
    WStype_FRAGMENT_TEXT_START, WStype_FRAGMENT_BIN_START, WStype_FRAGMENT, WStype_FRAGMENT_FIN, WStype_PING, WStype_PONG };
class WebSocketsClient {
public:
    static WebSocketsClient* latest;
    std::function<void(WStype_t,uint8_t*,size_t)> callback;
    std::vector<std::string> sent;
    bool connected = false;
    unsigned begins = 0, disconnects = 0, interval = 0, ping = 0, pong = 0, missed = 0;
    std::string host, path, protocol; uint16_t port = 0;
    WebSocketsClient() { latest = this; }
    void onEvent(std::function<void(WStype_t,uint8_t*,size_t)> value) { callback = value; }
    void setReconnectInterval(unsigned value) { interval = value; }
    void enableHeartbeat(uint32_t p, uint32_t t, uint8_t n) { ping=p; pong=t; missed=n; }
    void begin(const char* h, uint16_t p, const char* u, const char* protocolName) { ++begins; host=h;port=p;path=u;protocol=protocolName; }
    void loop() {}
    bool sendTXT(const char* text) { sent.emplace_back(text); return connected; }
    bool isConnected() const { return connected; }
    void disconnect() { ++disconnects; if (connected) emit(WStype_DISCONNECTED,""); connected=false; }
    void emit(WStype_t type,const char* data) {
        if (type == WStype_CONNECTED) connected=true;
        if (type == WStype_DISCONNECTED) connected=false;
        callback(type,reinterpret_cast<uint8_t*>(const_cast<char*>(data)),strlen(data));
    }
};
