#pragma once

#include <stdint.h>

struct DeviceConfig {
    char wifiSSID[33];     // 802.11 SSID max 32 bytes + NUL
    char wifiPassword[65]; // WPA2-PSK max 63 bytes + NUL (plus room for paste)
    char mqttBroker[64];
    char mqttUsername[32];
    char mqttPassword[64];
    char webUsername[32];
    char webPassword[64];
    float meterPsuVoltage;
    char ant1Name[32];
    char ant2Name[32];
    uint16_t mqttPort;
    bool mqttDiscoveryEnabled;
    uint8_t remoteUnitId;
    bool configured;
    // Static IP (optional; DHCP used when useStaticIP is false)
    bool useStaticIP;
    char staticIP[16];
    char staticNetmask[16];
    char staticGateway[16];
    char staticDNS[16];
    // Display-build only: hostname/IP of remote unit to subscribe to
    char remoteHost[64];
};
