#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "device_config.h"


class ConfigManager {
public:
    ConfigManager();

    bool begin();
    DeviceConfig get() const;
    bool update(const DeviceConfig& config);
    bool setupWiFi();
    bool reset();
    void serviceIdle();
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
    mutable portMUX_TYPE m_configMux = portMUX_INITIALIZER_UNLOCKED;
    void (*m_portalIdleCb)() = nullptr;

    void loadDefaults();
    bool save(const DeviceConfig& config);
    bool load();
};

// Global instance
extern ConfigManager configManager;

#endif // CONFIG_MANAGER_H
