#include <Arduino.h>
#include "config.h"
#include <WiFi.h>
#include <WiFiClient.h>
#if TLS_ENABLED
#include <WiFiClientSecure.h>
#include "portal_cert.h"
#endif
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config_manager.h"
#include "tuner_protocol.h"
#include "mqtt_handler.h"
#include "web_server.h"

#ifdef WITH_DISPLAY
#include "display_ui.h"
#endif

// Global instances
TunerProtocol tuner;
MqttHandler mqtt;
LdgWebServer webServer;

#ifdef WITH_DISPLAY
DisplayUI display;
static std::atomic<bool> s_remoteDisplayPending{false};

#if TLS_ENABLED
using WebClient = WiFiClientSecure;
#else
using WebClient = WiFiClient;
#endif
static WebClient s_remoteClient;
static bool             s_remoteConnected  = false;
static uint32_t         s_lastRemoteReconnect = 0;

static String buildBasicAuth(const char* user, const char* pass) {
    String src = String(user) + ":" + pass;
    const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    const uint8_t* bytes = (const uint8_t*)src.c_str();
    int len = src.length();
    for (int i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)bytes[i] << 16;
        if (i + 1 < len) n |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)bytes[i + 2];
        out += b64[(n >> 18) & 0x3F];
        out += b64[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? b64[(n >>  6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64[ n        & 0x3F] : '=';
    }
    return out;
}
#endif

// WiFi management
static uint32_t lastWiFiCheck = 0;
static bool wifiConnected = false;

// Remote snapshots may also arrive from the web-server task (POST telemetry).
static portMUX_TYPE s_activeMux = portMUX_INITIALIZER_UNLOCKED;
static ldg::ActiveTunerState s_activeState;
static uint32_t s_lastRemoteTelemetry = 0;

bool activeRemoteConnected() {
    portENTER_CRITICAL(&s_activeMux);
    const bool connected = s_activeState.remoteConnected();
    portEXIT_CRITICAL(&s_activeMux);
    return connected;
}

tuner_state_t getActiveState() {
    portENTER_CRITICAL(&s_activeMux);
    const bool remote = s_activeState.remoteConnected();
    const tuner_state_t state = s_activeState.select(tuner_state_t{});
    portEXIT_CRITICAL(&s_activeMux);
    return remote ? state : tuner.getState();
}

// ============================================================
// WiFi Setup (using WiFiManager)
// ============================================================

void setupWiFi() {
#ifdef WITH_DISPLAY
    const char* hostname = "ldgcontrol-display";
#else
    const char* hostname = "ldgcontrol-remote";
#endif
    WiFi.setHostname(hostname);

    // WiFiManager handles captive portal and credential storage
    bool connected = configManager.setupWiFi();

    if (connected) {
        wifiConnected = true;
        Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());

        if (MDNS.begin(hostname)) {
            Serial.printf("mDNS responder started: %s.local\n", hostname);
            MDNS.addService(TLS_ENABLED ? "https" : "http", "tcp", WEB_PORT);
        }
    } else {
        wifiConnected = false;
        Serial.println("\nWiFi connection failed or config portal timed out");
    }
}

// ============================================================
// Filesystem
// ============================================================

bool setupFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed, using embedded resources");
        return false;
    }
    Serial.println("LittleFS mounted");
    return true;
}

// ============================================================
// Remote Telemetry Callback (display unit)
// ============================================================

void onRemoteTelemetry(const tuner_state_t& state) {
    portENTER_CRITICAL(&s_activeMux);
    s_activeState.updateRemote(state);
    s_lastRemoteTelemetry = millis();
    portEXIT_CRITICAL(&s_activeMux);
#ifdef WITH_DISPLAY
    // POST telemetry runs on the web task; LVGL belongs to the main task.
    s_remoteDisplayPending.store(true);
#endif
    webServer.pushMeterEvent(getActiveState());
}

void onRemoteDisconnected() {
    portENTER_CRITICAL(&s_activeMux);
    s_activeState.disconnectRemote();
    portEXIT_CRITICAL(&s_activeMux);
    const tuner_state_t local = tuner.getState();
    webServer.pushMeterEvent(local);
#ifdef WITH_DISPLAY
    display.updateMeter(&local.meter);
    display.updateStatus(local.mode, local.antenna);
#endif
    Serial.println("Remote unit disconnected; using local tuner");
}

// ============================================================
// Local Meter Callback
// ============================================================

void onMeterUpdate(const tuner_meter_t* meter) {
    if (activeRemoteConnected()) return;

    // Push directly into the SSE state from the main task so subscribers keep
    // getting fresh readings even when the web-server task is currently inside
    // a long-running SSE handler (webServer.loop()'s own 500 ms poll can't
    // fire while a handler holds the slot).
    webServer.pushMeterEvent(tuner.getState());

#ifdef WITH_DISPLAY
    display.updateMeter(meter);
    display.updateStatus(tuner.getMode(), tuner.getAntenna());
#endif
}

// ============================================================
// Remote Unit SSE Client (display build only)
// ============================================================

#ifdef WITH_DISPLAY
void connectRemoteUnit() {
    if (s_remoteConnected || !wifiConnected) return;
    uint32_t now = millis();
    if (now - s_lastRemoteReconnect < 5000) return;
    s_lastRemoteReconnect = now;

    const DeviceConfig& cfg = configManager.get();
    if (cfg.remoteHost[0] == '\0') return;

    IPAddress remoteIP;
    const char* host = cfg.remoteHost;
    bool resolved = false;

    if (remoteIP.fromString(host)) {
        resolved = true;
    } else if (WiFi.hostByName(host, remoteIP)) {
        resolved = true;
    }
    if (!resolved) {
        Serial.printf("Remote: can't resolve %s\n", host);
        return;
    }

    Serial.printf("Connecting to remote SSE at %s:%d...\n",
                  remoteIP.toString().c_str(), WEB_PORT);
#if TLS_ENABLED
    s_remoteClient.setInsecure();
#endif

    if (!s_remoteClient.connect(remoteIP, WEB_PORT)) {
        Serial.println("Remote: connect failed");
        return;
    }

    String auth = buildBasicAuth(cfg.webUsername, cfg.webPassword);
    s_remoteClient.printf(
        "GET /api/events HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Basic %s\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        remoteIP.toString().c_str(),
        auth.c_str()
    );
    s_remoteConnected = true;
    Serial.println("Remote: SSE connected");
}

void processRemoteSSE() {
    portENTER_CRITICAL(&s_activeMux);
    const bool stale = s_activeState.remoteConnected() &&
        millis() - s_lastRemoteTelemetry > HEARTBEAT_INTERVAL_MS * 2;
    portEXIT_CRITICAL(&s_activeMux);
    if (stale) {
        s_remoteClient.stop();
        s_remoteConnected = false;
        onRemoteDisconnected();
    }
    if (!s_remoteConnected || !s_remoteClient.connected()) {
        if (s_remoteConnected) {
            Serial.println("Remote: SSE disconnected");
            s_remoteClient.stop();
            s_remoteConnected = false;
            onRemoteDisconnected();
        }
        return;
    }

    while (s_remoteClient.available()) {
        String line = s_remoteClient.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("data:")) continue;

        String payload = line.substring(5);
        payload.trim();
        JsonDocument doc;
        if (deserializeJson(doc, payload)) continue;

        if (doc.is<JsonObject>() && !doc["fwd_power"].isUnbound()) {
            onRemoteTelemetry(ldg::readWebState(doc.as<JsonVariantConst>()));
        }
    }
}

bool proxyCommandToRemote(uint8_t cmd) {
    const DeviceConfig& cfg = configManager.get();
    if (cfg.remoteHost[0] == '\0') return false;

    IPAddress remoteIP;
    if (!remoteIP.fromString(cfg.remoteHost)) {
        if (!WiFi.hostByName(cfg.remoteHost, remoteIP)) return false;
    }

    const char* endpoint = nullptr;
    switch (cmd) {
        case TUNER_CMD_TOGGLE_ANT:  endpoint = "/api/command/toggle";      break;
        case TUNER_CMD_MEM_TUNE:    endpoint = "/api/command/memory_tune"; break;
        case TUNER_CMD_FULL_TUNE:   endpoint = "/api/command/full_tune";   break;
        case TUNER_CMD_BYPASS:      endpoint = "/api/command/bypass";      break;
        case TUNER_CMD_AUTO_MODE:   endpoint = "/api/command/auto";        break;
        case TUNER_CMD_MANUAL_MODE: endpoint = "/api/command/manual";      break;
        default: return false;
    }

    WebClient client;
#if TLS_ENABLED
    client.setInsecure();
#endif
    if (!client.connect(remoteIP, WEB_PORT)) return false;

    String auth = buildBasicAuth(cfg.webUsername, cfg.webPassword);
    client.printf(
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        endpoint,
        remoteIP.toString().c_str(),
        auth.c_str()
    );

    uint32_t deadline = millis() + 3000;
    while (!client.available() && millis() < deadline) delay(10);
    String status = client.readStringUntil('\n');
    client.stop();
    return status.indexOf("200") >= 0;
}
#endif // WITH_DISPLAY

// ============================================================
// Command Execution (for web/MQTT)
// ============================================================

bool executeCommand(uint8_t cmd) {
#ifdef WITH_DISPLAY
    if (activeRemoteConnected()) {
        const bool result = proxyCommandToRemote(cmd);
        mqtt.requestStatePublish();
        return result;
    }
#endif
    bool result = false;
    switch (cmd) {
        case TUNER_CMD_TOGGLE_ANT:  result = tuner.toggleAntenna(); break;
        case TUNER_CMD_MEM_TUNE:    result = tuner.memoryTune();    break;
        case TUNER_CMD_FULL_TUNE:   result = tuner.fullTune();      break;
        case TUNER_CMD_BYPASS:      result = tuner.bypass();        break;
        case TUNER_CMD_AUTO_MODE:   result = tuner.setAutoMode();   break;
        case TUNER_CMD_MANUAL_MODE: result = tuner.setManualMode(); break;
    }
    mqtt.requestStatePublish();
    return result;
}

// ============================================================
// Setup
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("\n=== LDG Tuner Controller ===");
    Serial.printf("Build: %s %s\n",
#ifdef WITH_DISPLAY
        "display"
#else
        "remote"
#endif
    , __DATE__);
    Serial.printf("Web transport: %s (TLS_ENABLED=%d, port=%u)\n",
                  TLS_ENABLED ? "HTTPS" : "HTTP", TLS_ENABLED, WEB_PORT);

    // Load configuration from NVS
    configManager.begin();

    setupFS();

#if TLS_ENABLED
    if (!portalCert_init()) {
        Serial.println("ERROR: portal cert init failed; HTTPS will not work");
    }
#endif

    // Tuner and display are UART/SPI — no WiFi dependency. Init them early so
    // the display is live during portal setup and for users who never use WiFi.
    Serial.println("Initializing tuner interface...");
    if (!tuner.begin(TUNER_RX_PIN, TUNER_TX_PIN, TUNER_SERIAL_BAUD)) {
        Serial.println("ERROR: Failed to initialize tuner serial");
    } else {
        Serial.println("Tuner interface ready");
    }

    tuner.setMeterCallback(onMeterUpdate);

#ifdef WITH_DISPLAY
    Serial.println("Initializing display...");
    display.begin(&tuner);
#endif

    // The WiFi setup portal may run indefinitely. Initialize MQTT before it,
    // independently of STA status, and service it from the portal's idle loop.
    static WiFiClient mqttClient;
    mqtt.begin(mqttClient, executeCommand, getActiveState);
    configManager.setPortalIdleCallback([]() {
        tuner.process();
        mqtt.loop();
#ifdef WITH_DISPLAY
        display.loop();
#endif
    });

    setupWiFi();

#ifdef WITH_DISPLAY
    display.updateNetworkInfo();
#endif

    if (!webServer.begin(&tuner, executeCommand, getActiveState,
#ifdef WITH_DISPLAY
                         onRemoteTelemetry,
#else
                         nullptr,
#endif
                         []() { return mqtt.getStatus(); })) {
        Serial.printf("ERROR: web server failed to start - port %u may still be bound\n", WEB_PORT);
    }

    // Spin the runtime web server on a dedicated FreeRTOS task so its
    // handlers — especially the long-running SSE stream — cannot block
    // loop()'s tuner.process(). Without this, an active SSE subscriber
    // freezes the meter and queues every POST behind itself until the
    // SSE session cap fires. UART access shared between this task and
    // loop() is serialized by TunerProtocol's mutex. The 16 KB stack is
    // retained for the SSE handlers and ArduinoJson serialization.
    xTaskCreatePinnedToCore(
        [](void*) {
            for (;;) {
                webServer.loop();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        },
        "webServer", 16384, NULL, 5, NULL, 1);

    Serial.println("=== System Ready ===");
}

// ============================================================
// Loop
// ============================================================

void loop() {
    tuner.process();

    // WiFi reconnection — only manage STA when STA is actually configured.
    // In AP-only mode (captive portal still running, never configured) there
    // is no STA to reconnect, and WiFi.status() returns disconnected forever.
    if (millis() - lastWiFiCheck > 10000) {
        lastWiFiCheck = millis();
        wifi_mode_t mode = WiFi.getMode();
        bool staActive = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
        if (!staActive) {
            wifiConnected = false;
        } else if (WiFi.status() != WL_CONNECTED) {
            if (wifiConnected) {
                Serial.println("WiFi disconnected, attempting reconnect...");
            }
            wifiConnected = false;
            WiFi.reconnect();
        } else if (!wifiConnected) {
            wifiConnected = true;
            Serial.println("WiFi reconnected");
#ifdef WITH_DISPLAY
            display.updateNetworkInfo();
#endif
        }
    }

    // webServer.loop() is now driven by its own FreeRTOS task; see setup().

#ifdef WITH_DISPLAY
    connectRemoteUnit();
    processRemoteSSE();
    if (s_remoteDisplayPending.exchange(false)) {
        const tuner_state_t state = getActiveState();
        tuner_meter_t meter = state.meterValid ? state.meter : tuner_meter_t{};
        // Keep invalid readings unknown on MQTT; the analog gauge needs finite coordinates.
        if (!std::isfinite(meter.forward_power_watts)) meter.forward_power_watts = 0;
        if (!std::isfinite(meter.reflected_power_watts)) meter.reflected_power_watts = 0;
        if (!std::isfinite(meter.swr) || !state.meterValid) meter.swr = 1;
        display.updateMeter(&meter);
        display.updateStatus(state.mode, state.antenna);
    }
    display.loop();
#endif

    mqtt.loop();
    delay(1);
}
