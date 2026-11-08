#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <stdint.h>

namespace ldg {
// Physical USB only. Never routed to the tuner's UART. Sequential requests
// retain the exact last request/response so a lost save reply is safe to retry.
class UsbProvisioning {
public:
    static constexpr size_t Limit = 1536;
    using Handler = void (*)(const char*, JsonVariantConst, JsonDocument&);
    void begin(const char* boot, Handler handler) { strncpy(m_boot,boot,16); m_boot[16]=0; m_handler=handler; m_session=m_lastCid=0; m_client[0]=m_lastRequest[0]=m_lastReply[0]=0; }
    bool handle(const char* line, char* output, size_t capacity) {
        JsonDocument request, reply;
        if (strlen(line) > Limit || deserializeJson(request,line) || !request["ldg"].is<unsigned>() || request["ldg"].as<unsigned>() != 1) return false;
        const uint32_t cid = request["cid"] | 0u;
        const char* op = request["op"] | "";
        reply["ldg"]=1; reply["cid"]=cid; reply["boot"]=m_boot; reply["sid"]=m_session;
        if (!cid || !request["cid"].is<uint32_t>()) { reply["status"]=400; reply["error"]="invalid cid"; }
        else if (!strcmp(op,"hello")) {
            const char* client=request["client"] | "";
            if (!validClient(client) || request["client"].as<JsonString>().size()!=32) {reply["status"]=400;reply["error"]="invalid client";}
            else if (!strcmp(client,m_client) && cid<=m_lastCid) {
                if (cid==m_lastCid && !strcmp(line,m_lastRequest)) {
                    if (strlen(m_lastReply)>=capacity) return false;
                    strcpy(output,m_lastReply);return true;
                }
                reply["status"]=409;reply["error"]="old hello cid";
            }
            else {
                if (strcmp(client,m_client)) { strcpy(m_client,client); if (!++m_session) ++m_session; m_lastCid=0; }
                reply["sid"]=m_session; reply["status"]=200;
                if (m_handler) m_handler(op,request.as<JsonVariantConst>(),reply);
            }
        } else if (!m_session || request["boot"].as<JsonString>().size()!=16 || !request["sid"].is<uint32_t>() ||
                   strcmp(request["boot"] | "",m_boot) || request["sid"].as<uint32_t>() != m_session) {
            reply["status"]=409; reply["error"]="boot or session changed";
        } else if (cid == m_lastCid) {
            if (!strcmp(line,m_lastRequest)) {if (strlen(m_lastReply)>=capacity) return false; strcpy(output,m_lastReply); return true;}
            reply["status"]=409;reply["error"]="cid body changed";
        } else if (cid < m_lastCid) {reply["status"]=409;reply["error"]="old cid";}
        else {
            reply["status"]=200;
            if (m_handler) m_handler(op,request.as<JsonVariantConst>(),reply);
            else {reply["status"]=503;reply["error"]="not ready";}
        }
        if (reply.overflowed() || measureJson(reply)>=capacity || measureJson(reply)>Limit) return false;
        serializeJson(reply,output,capacity);
        // Invalid ownership/replay must never displace the retry entry.
        if (cid && reply["status"].as<int>() != 409 && m_session &&
            (!strcmp(op,"hello") || (!strcmp(request["boot"] | "",m_boot) && request["sid"].as<uint32_t>() == m_session))) {
            m_lastCid=cid; strcpy(m_lastRequest,line); strcpy(m_lastReply,output);
        }
        return true;
    }
private:
    static bool validClient(const char* text) {
        if (strlen(text)!=32) return false;
        for (;*text;++text) if (!((*text>='0'&&*text<='9')||(*text>='a'&&*text<='f'))) return false;
        return true;
    }
    Handler m_handler=nullptr;
    char m_boot[17]{},m_client[33]{},m_lastRequest[Limit+1]{},m_lastReply[Limit+1]{};
    uint32_t m_session=0,m_lastCid=0;
};
}
