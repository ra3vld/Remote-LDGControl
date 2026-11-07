#pragma once

#include <stdint.h>
#include <stddef.h>

template<size_t MqttUserBytes, size_t MqttPasswordBytes> struct DeviceConfigLayout {
    char wifiSSID[33];     // 802.11 SSID max 32 bytes + NUL
    char wifiPassword[65]; // WPA2-PSK max 63 bytes + NUL (plus room for paste)
    char mqttBroker[64];
    char mqttUsername[MqttUserBytes];
    char mqttPassword[MqttPasswordBytes];
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
    // V2 used tail padding for orientation; keep that legacy layout readable.
    uint8_t displayOrientation = 0;
};
using DeviceConfigV2 = DeviceConfigLayout<32, 64>;
using DeviceConfig = DeviceConfigLayout<65, 129>;
static_assert(sizeof(DeviceConfigV2) == 560, "Keep the legacy NVS layout readable");
static_assert(sizeof(DeviceConfig) == 656, "Check configuration layout changes");
