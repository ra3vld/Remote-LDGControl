#pragma once
#include "WiFi.h"
static constexpr int ESP_OK = 0;
static constexpr int ESP_MAC_WIFI_STA = 0;
inline int esp_read_mac(uint8_t* mac, int) {
    if (!WiFi.readMacOk) return -1;
    memcpy(mac, WiFi.stationMac, sizeof(WiFi.stationMac));
    return ESP_OK;
}
