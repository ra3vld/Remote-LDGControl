#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <cstring>

namespace ldg {
struct BridgeState {
    char sourceToken[33]{};
    uint32_t uptimeMs = 0, meterAgeMs = UINT32_MAX;
    bool busy = false, connected = false, remote = false;
};
struct BridgeRequest {
    char requestId[37]{}, sourceToken[33]{}, targetHost[64]{};
    uint32_t expiresMs = 0;
    uint8_t command = 0;
};
using BridgeGetter = BridgeState (*)();
using BridgeHandler = const char* (*)(const BridgeRequest&);

inline int hexDigit(char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
inline bool parseHex(const char* text, uint8_t* bytes, unsigned count, bool uuid = false) {
    if (!text || strlen(text) != (uuid ? 36u : count*2)) return false;
    unsigned p = 0;
    for (unsigned i=0; i<count; ++i) {
        if (uuid && (p==8 || p==13 || p==18 || p==23)) { if (text[p++]!='-') return false; }
        const int a=hexDigit(text[p++]), b=hexDigit(text[p++]);
        if (a<0 || b<0) return false;
        bytes[i]=uint8_t((a<<4)|b);
    }
    return true;
}
inline bool parseBridgeRequest(JsonVariantConst doc, BridgeRequest& request) {
    const char* id=doc["request_id"].as<const char*>();
    const char* token=doc["source_token"].as<const char*>();
    uint8_t scratch[16];
    if (!parseHex(id,scratch,16,true) || !parseHex(token,scratch,16) || !doc["expires_uptime_ms"].is<uint32_t>()) return false;
    strcpy(request.requestId,id); strcpy(request.sourceToken,token);
    request.expiresMs=doc["expires_uptime_ms"].as<uint32_t>();
    return true;
}
inline const char* validateBridgeRequest(const BridgeRequest& request, const BridgeState& state) {
    if (!state.sourceToken[0] || strcmp(request.sourceToken,state.sourceToken)) return "target_changed";
    const int32_t remaining=int32_t(request.expiresMs-state.uptimeMs);
    if (remaining<=0 || remaining>2000) return "expired";
    return nullptr;
}
inline void writeBridgeState(JsonDocument& doc, const BridgeState& state) {
    doc["api_version"]=2; doc["source_token"]=state.sourceToken; doc["uptime_ms"]=state.uptimeMs;
    doc["busy"]=state.busy; doc["connected"]=state.connected; doc["meter_age_ms"]=state.meterAgeMs;
}
// Bounded RAM, shared across MQTT session reconstruction. No heap strings.
class BridgeCache {
    struct Entry {
        uint8_t id[16]{}, token[16]{};
        uint32_t expires=0;
        uint8_t command=0;
        const char* result=nullptr;
    } m_entries[16];
    unsigned m_next=0;
public:
    const char* find(const BridgeRequest& request) const {
        uint8_t id[16],token[16];
        if (!parseHex(request.requestId,id,16,true) || !parseHex(request.sourceToken,token,16)) return "invalid_request";
        for (const auto& entry:m_entries) if (entry.result && !memcmp(entry.id,id,16))
            return entry.command==request.command && entry.expires==request.expiresMs && !memcmp(entry.token,token,16) ? entry.result : "duplicate_conflict";
        return nullptr;
    }
    void save(const BridgeRequest& request,const char* result) {
        auto& entry=m_entries[m_next++%16];
        parseHex(request.requestId,entry.id,16,true); parseHex(request.sourceToken,entry.token,16);
        entry.expires=request.expiresMs; entry.command=request.command; entry.result=result;
    }
};
}
