#pragma once
#include "config_store.h"
#include <ArduinoJson.h>

namespace ldg {
inline bool validConfigText(const char* text) {
    const auto* p=reinterpret_cast<const uint8_t*>(text);
    while (*p) {
        uint32_t cp=*p++;unsigned count=0;
        if (cp<128) {if (cp<32 || cp==127) return false;continue;}
        const uint32_t minimum=cp>=0xf0?0x10000:cp>=0xe0?0x800:0x80;
        if (cp>=0xc2 && cp<=0xdf) {cp&=31;count=1;}
        else if (cp>=0xe0 && cp<=0xef) {cp&=15;count=2;}
        else if (cp>=0xf0 && cp<=0xf4) {cp&=7;count=3;}
        else return false;
        while (count--) {if ((*p&0xc0)!=0x80) return false;cp=(cp<<6)|(*p++&63);}
        if (cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
template<size_t N> inline bool patchText(JsonVariantConst doc, const char* key, char (&target)[N], bool keepEmpty = false) {
    const auto value = doc[key];
    if (value.isUnbound()) return true;
    if (!value.is<const char*>()) return false;
    const auto text = value.as<JsonString>();
    if (text.size() >= N || strlen(text.c_str()) != text.size()) return false;
    if (!validConfigText(text.c_str())) return false;
    if (!keepEmpty || text.size()) memcpy(target, text.c_str(), text.size() + 1);
    return true;
}

// Shared by HTTP settings and the physical USB provisioning adapter.
inline bool patchConnectionConfig(JsonVariantConst doc, DeviceConfig& cfg, bool wifi = false) {
    if (!doc.is<JsonObjectConst>()) return false;
    if (!patchText(doc,"mqttBroker",cfg.mqttBroker) || !patchText(doc,"mqttUsername",cfg.mqttUsername) ||
        !patchText(doc,"mqttPassword",cfg.mqttPassword,true) || !patchText(doc,"staticIP",cfg.staticIP) ||
        !patchText(doc,"staticNetmask",cfg.staticNetmask) || !patchText(doc,"staticGateway",cfg.staticGateway) ||
        !patchText(doc,"staticDNS",cfg.staticDNS)) return false;
    for (const char* key : {"useStaticIP","mqttDiscoveryEnabled","clearMqttPassword","clearWifiPassword"})
        if (!doc[key].isUnbound() && !doc[key].is<bool>()) return false;
    if (!doc["mqttPort"].isUnbound()) {
        if (!doc["mqttPort"].is<uint16_t>() || !doc["mqttPort"].as<uint16_t>()) return false;
        cfg.mqttPort = doc["mqttPort"];
    }
    if (!doc["mqttDiscoveryEnabled"].isUnbound()) cfg.mqttDiscoveryEnabled = doc["mqttDiscoveryEnabled"];
    if (!doc["useStaticIP"].isUnbound()) cfg.useStaticIP = doc["useStaticIP"];
    if (doc["clearMqttPassword"] == true) cfg.mqttPassword[0] = 0;
    if (wifi) {
        if (!patchText(doc,"wifiSSID",cfg.wifiSSID) || !patchText(doc,"wifiPassword",cfg.wifiPassword,true)) return false;
        if (doc["clearWifiPassword"] == true) cfg.wifiPassword[0] = 0;
        if (!doc["wifiSSID"].isUnbound() && !cfg.wifiSSID[0]) return false;
        const size_t length = strlen(cfg.wifiPassword);
        if (cfg.wifiSSID[0] && length && (length < 8 || length > 64)) return false;
        if (length == 64) for (const char* p = cfg.wifiPassword; *p; ++p)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
    }
    if (cfg.mqttBroker[0]) for (const char* p = cfg.mqttBroker; *p; ++p)
        if (uint8_t(*p) <= 32 || uint8_t(*p) >= 127 || strchr("/#+@",*p)) return false;
    return validDeviceConfig(cfg);
}
}
