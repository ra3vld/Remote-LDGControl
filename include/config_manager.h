#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

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

class ConfigManager {
public:
    ConfigManager();

    bool begin();
    const DeviceConfig& get() const;
    bool update(const DeviceConfig& config);
    bool setupWiFi();
    void reset();
    bool isConfigured() const;

    // Captive portal web server
    void startPortalServer();
    void stopPortalServer();
    
    // Portal content
    String portalHTML();
    String scanNetworksJSON();

    // Called each iteration of the portal idle loop (replaces delay).
    // Use this to keep hardware (display, tuner) alive while waiting for WiFi config.
    void setPortalIdleCallback(void (*cb)()) { m_portalIdleCb = cb; }

private:
    Preferences m_prefs;
    DeviceConfig m_config;
    void (*m_portalIdleCb)() = nullptr;

    void loadDefaults();
    bool save();
    bool load();
};

// Global instance
extern ConfigManager configManager;

#endif // CONFIG_MANAGER_H
