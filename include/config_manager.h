#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "device_config.h"
#include "antenna_settings.h"
#include "meter_scale.h"
#include "brightness_control.h"


class ConfigManager {
public:
    ConfigManager();

    bool begin();
    DeviceConfig get() const;
    bool update(const DeviceConfig& config);
    ldg::AntennaSettings antennaSettings() const;
    bool updateAntennaSettings(const ldg::AntennaSettings& settings);
    ldg::MeterSettings meterSettings() const;
    bool updateMeterSettings(const ldg::MeterSettings& settings);
    ldg::BrightnessSettings brightnessSettings() const;
    bool updateBrightnessSettings(const ldg::BrightnessSettings& settings);
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
    ldg::AntennaSettings m_antennaSettings;
    ldg::MeterSettings m_meterSettings;
    ldg::BrightnessSettings m_brightnessSettings;
    mutable portMUX_TYPE m_configMux = portMUX_INITIALIZER_UNLOCKED;
    void (*m_portalIdleCb)() = nullptr;

    void loadDefaults();
    bool save(const DeviceConfig& config);
    bool load();
};

// Global instance
extern ConfigManager configManager;

#endif // CONFIG_MANAGER_H
