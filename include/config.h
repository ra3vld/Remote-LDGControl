#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================
// WiFi Configuration
// ============================================================
#define WIFI_STA_SSID           "your_network_ssid"
#define WIFI_STA_PASSWORD       "your_network_password"
#define WIFI_AP_SSID            "LDGControl"
#define WIFI_AP_PASSWORD        "ldgcontrol"
#define WIFI_AP_CHANNEL         6
#define WIFI_CONNECT_TIMEOUT_MS 15000

// ============================================================
// Network Services
// ============================================================
#define HTTP_PORT               80
#define HTTPS_PORT              443
#define MQTT_PORT               1883

// Optional web/portal/remote-link TLS. Override with -DTLS_ENABLED=1.
#ifndef TLS_ENABLED
#define TLS_ENABLED             0
#endif
#if TLS_ENABLED != 0 && TLS_ENABLED != 1
#error "TLS_ENABLED must be 0 or 1"
#endif
#if TLS_ENABLED
#define WEB_PORT                HTTPS_PORT
#define WEB_SCHEME              "https://"
#else
#define WEB_PORT                HTTP_PORT
#define WEB_SCHEME              "http://"
#endif

// ============================================================
// MQTT Configuration
// ============================================================
#define MQTT_BROKER             "192.168.1.100"
#define MQTT_CLIENT_PREFIX      "ldg-tuner"
#define MQTT_USERNAME           "ldgcontroller"
#define MQTT_PASSWORD           "change_me"
#define MQTT_QOS                1
#define MQTT_RETAIN_TELEMETRY   false
#define LDG_MQTT_KEEPALIVE      60

// MQTT topics and Home Assistant identities are generated per-device from MAC.

// ============================================================
// Serial (Tuner) Configuration
// ============================================================
#define TUNER_SERIAL_BAUD       38400
#ifdef BOARD_ESP32_3248S035C
// P3 exposes GPIO35 (input only) and GPIO22. GPIO16/17 drive the RGB LED.
#define TUNER_RX_PIN            35
#define TUNER_TX_PIN            22
#else
#define TUNER_RX_PIN            16  // UART2 RX - connect to tuner TX
#define TUNER_TX_PIN            17  // UART2 TX - connect to tuner RX
#endif
#define TUNER_UART_NUM          UART_NUM_2
#define TUNER_CMD_DELAY_MS      250
#define TUNER_WAKE_DELAY_MS     1

// ============================================================
// Display Configuration (WITH_DISPLAY builds)
// ============================================================
#ifdef WITH_DISPLAY
#include "User_Setup.h"
#define DISPLAY_WIDTH           480
#define DISPLAY_HEIGHT          320

#ifdef BOARD_ESP32_3248S035C
// ST7796 MADCTL MV: normal landscape. Rotation 7 adds MY and mirrors X.
#define DISPLAY_ROTATION        1
#define TOUCH_GT911
#define TOUCH_SDA               33
#define TOUCH_SCL               32
#define TOUCH_RST               25
// Poll over I2C; do not drive INT (grounded on some stock revisions).
#else
#define DISPLAY_ROTATION        1

// BTT TFT35-SPI V2.1 (ILI9488 display + NS2009 I2C touch)
// JST-XH 10-pin: IORQ, SCL, SDA, RS, NSS, SCK, MOSI, MISO, GND, +5V
// Touch (NS2009) - uses I2C, not SPI
#define TOUCH_SDA               21    // Pin 3 - SDA (I2C data)
#define TOUCH_SCL               22    // Pin 2 - SCL (I2C clock)
#define TOUCH_IRQ               39    // Pin 1 - IORQ (touch interrupt, optional)
#endif
#endif

// ============================================================
// Security Configuration
// ============================================================
#define WEB_AUTH_ENABLED        true
#define WEB_AUTH_USERNAME       "admin"
#define WEB_AUTH_PASSWORD       "change_me_please"
#define WEB_RATE_LIMIT_REQUESTS 10
#define WEB_RATE_LIMIT_WINDOW_MS 1000
#define MAX_LOGIN_ATTEMPTS      5
#define LOGIN_LOCKOUT_MS        300000  // 5 minutes

// ============================================================
// System
// ============================================================
#define HOSTNAME_PREFIX         "ldg-tuner"
#define OTA_PASSWORD            "ota_password_change_me"
#define NTP_SERVER1             "pool.ntp.org"
#define NTP_SERVER2             "time.nist.gov"
#define NTP_TIMEZONE            "EST5EDT,M3.2.0,M11.1.0"
#define HEARTBEAT_INTERVAL_MS   30000

// ============================================================
// Meter Calculation
// ============================================================
#define METER_PSU_VOLTAGE       13.8  // Default/fallback PSU voltage
#define METER_POWER_SCALE       1000  // 1000 for AT-1000ProII, 600 for AT-600ProII
#define METER_LOW_SCALE         100   // Low range scale (100W for both models)

// Voltage Sense (ADC) - auto-detect PSU voltage
#ifdef BOARD_ESP32_3248S035C
// GPIO34 belongs to the onboard light sensor; no free ADC for PSU sensing.
#define VOLTAGE_SENSE_PIN       -1
#else
#define VOLTAGE_SENSE_PIN       34    // GPIO 34 (ADC1_CH6, input-only)
#endif
#define VOLTAGE_DIVIDER_RATIO   5.545 // (R1+R2)/R2 = (100k+22k)/22k
#define VOLTAGE_SAMPLES         32    // Number of ADC samples to average

#endif // CONFIG_H
