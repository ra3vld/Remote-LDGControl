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
#include <HTTPClient.h>
#include <freertos/queue.h>
#include "voltage_sensor.h"
#include "remote_protocol.h"
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
ldg::MeterScale meterScale;
static std::atomic<bool> s_webReady{false};

#ifdef WITH_DISPLAY
DisplayUI display;

#if TLS_ENABLED
using WebClient = WiFiClientSecure;
#else
using WebClient = WiFiClient;
#endif
static uint32_t s_lastRemotePoll = 0;
#endif

// WiFi management
static uint32_t lastWiFiCheck = 0;
static bool wifiConnected = false;

struct CommandRequest {
    uint8_t command;
    bool remote;
    SemaphoreHandle_t completed;
    bool* result;
};
static QueueHandle_t s_commands = nullptr;
#ifdef WITH_DISPLAY
static QueueHandle_t s_remoteCommands = nullptr;
#endif
static std::atomic<unsigned> s_pendingCommands{0};

// Remote snapshots may also arrive from the web-server task (POST telemetry).
static portMUX_TYPE s_activeMux = portMUX_INITIALIZER_UNLOCKED;
static ldg::ActiveTunerState s_activeState;
static uint32_t s_lastRemoteTelemetry = 0;
static bool s_remoteCommandActive = false; // Protected by s_activeMux.

bool activeRemoteConnected() {
    portENTER_CRITICAL(&s_activeMux);
    const bool connected = s_activeState.remoteConnected(
        millis() - s_lastRemoteTelemetry, s_remoteCommandActive);
    portEXIT_CRITICAL(&s_activeMux);
    return connected;
}

tuner_state_t getActiveState() {
    portENTER_CRITICAL(&s_activeMux);
    const tuner_state_t state = s_activeState.select(tuner_state_t{},
        millis() - s_lastRemoteTelemetry, s_remoteCommandActive);
    portEXIT_CRITICAL(&s_activeMux);
    return state.remote ? state : tuner.getState();
}

// ============================================================
// Wi-Fi setup
// ============================================================

void setupWiFi() {
#ifdef WITH_DISPLAY
    const char* hostname = "ldgcontrol-display";
#else
    const char* hostname = "ldgcontrol-remote";
#endif
    WiFi.setHostname(hostname);

    // ConfigManager owns the captive portal and credential storage.
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
    if (s_webReady.load()) webServer.pushMeterEvent(getActiveState());
}

void onRemoteDisconnected() {
    portENTER_CRITICAL(&s_activeMux);
    s_activeState.disconnectRemote();
    portEXIT_CRITICAL(&s_activeMux);
    const tuner_state_t local = tuner.getState();
    if (s_webReady.load()) webServer.pushMeterEvent(local);
    Serial.println("Remote unit disconnected; using local tuner");
}

// ============================================================
// Local Meter Callback
// ============================================================

void onMeterUpdate(const tuner_meter_t*) {
    if (activeRemoteConnected()) return;

    // The UART worker publishes value snapshots; it never calls LVGL.
    if (s_webReady.load()) webServer.pushMeterEvent(tuner.getState());

}

// ============================================================
// Remote unit polling (network task only)
// ============================================================

#ifdef WITH_DISPLAY
static bool remoteRequest(uint8_t command, JsonDocument& response) {
    const DeviceConfig cfg = configManager.get();
    if (!cfg.remoteHost[0] || WiFi.status() != WL_CONNECTED) return false;
    WebClient client;
#if TLS_ENABLED
    client.setInsecure();
#endif
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(ldg::remoteRequestTimeout(command));
    http.useHTTP10(true);
    const char* endpoint = command ? ldg::commandEndpoint(command) : "/api/status";
    if (!endpoint || !http.begin(client, String(WEB_SCHEME) + cfg.remoteHost + endpoint)) return false;
    http.setAuthorization(cfg.webUsername, cfg.webPassword);
    const int code = command ? http.POST("") : http.GET();
    bool ok = false;
    if (code == 200 && http.getSize() <= 4096) {
        const String body = http.getString();
        ok = body.length() <= 4096 && !deserializeJson(response, body) && response.is<JsonObject>();
        if (ok && command) ok = ldg::remoteCommandSucceeded(code, response.as<JsonVariantConst>());
    }
    http.end();
    return ok;
}

static void pollRemoteUnit() {
    const uint32_t now = millis();
    if (now - s_lastRemotePoll < 500) return;
    s_lastRemotePoll = now;
    const DeviceConfig cfg = configManager.get();
    if (!cfg.remoteHost[0] || WiFi.status() != WL_CONNECTED) {
        if (activeRemoteConnected()) onRemoteDisconnected();
        return;
    }
    JsonDocument response;
    if (remoteRequest(0, response) && ldg::validRemoteStatus(response.as<JsonVariantConst>())) {
        onRemoteTelemetry(ldg::readWebState(response.as<JsonVariantConst>()));
    } else {
        portENTER_CRITICAL(&s_activeMux);
        const bool wasRemote = s_activeState.remoteConnected();
        const bool stale = millis() - s_lastRemoteTelemetry > 3000;
        portEXIT_CRITICAL(&s_activeMux);
        if (wasRemote && stale) onRemoteDisconnected();
    }
}
#endif

// ============================================================
// Command Execution (for web/MQTT)
// ============================================================

static bool executeCommandNow(uint8_t cmd, bool remote) {
#ifdef WITH_DISPLAY
    if (remote) {
        if (!activeRemoteConnected()) return false;
        // Polling shares this worker with commands. Keep its source selected
        // while a long tune runs, but let old meter readings expire normally.
        portENTER_CRITICAL(&s_activeMux);
        s_remoteCommandActive = true;
        portEXIT_CRITICAL(&s_activeMux);
        JsonDocument response;
        const bool result = remoteRequest(cmd, response);
        pollRemoteUnit();
        portENTER_CRITICAL(&s_activeMux);
        s_remoteCommandActive = false;
        portEXIT_CRITICAL(&s_activeMux);
        mqtt.requestStatePublish();
        return result;
    }
#endif
    bool result = false;
    switch (cmd) {
        case TUNER_CMD_TOGGLE_ANT:  result = tuner.toggleAntenna(); break;
        case TUNER_CMD_SELECT_ANT_A: result = tuner.selectAntenna(ANT_A); break;
        case TUNER_CMD_SELECT_ANT_B: result = tuner.selectAntenna(ANT_B); break;
        case TUNER_CMD_MEM_TUNE:    result = tuner.memoryTune();    break;
        case TUNER_CMD_FULL_TUNE:   result = tuner.fullTune();      break;
        case TUNER_CMD_BYPASS:      result = tuner.bypass();        break;
        case TUNER_CMD_AUTO_MODE:   result = tuner.setAutoMode();   break;
        case TUNER_CMD_MANUAL_MODE: result = tuner.setManualMode(); break;
    }
    mqtt.requestStatePublish();
    return result;
}

// Synchronous callers wait on their own completion, never on the UI task.
static bool submitCommand(uint8_t command, bool wait) {
    const bool remote = activeRemoteConnected();
    QueueHandle_t destination = s_commands;
#ifdef WITH_DISPLAY
    if (remote) destination = s_remoteCommands;
#endif
    if (!destination) return false;
    SemaphoreHandle_t completed = wait ? xSemaphoreCreateBinary() : nullptr;
    if (wait && !completed) return false;
    bool result = false;
    CommandRequest request{command, remote, completed, wait ? &result : nullptr};
    s_pendingCommands.fetch_add(1);
    if (xQueueSend(destination, &request, 0) != pdTRUE) {
        s_pendingCommands.fetch_sub(1);
        if (completed) vSemaphoreDelete(completed);
        return false;
    }
    if (!wait) return true;
    xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return result;
}

bool executeCommand(uint8_t command) { return submitCommand(command, true); }
#ifdef WITH_DISPLAY
static bool enqueueDisplayCommand(uint8_t command) { return submitCommand(command, false); }
#endif

static void completeCommand(const CommandRequest& request) {
    const bool result = executeCommandNow(request.command, request.remote);
    if (request.result) *request.result = result;
    s_pendingCommands.fetch_sub(1);
    if (request.completed) xSemaphoreGive(request.completed);
}

static void tunerTask(void*) {
    for (;;) {
        tuner.process();
        CommandRequest request;
        if (xQueueReceive(s_commands, &request, 0) == pdTRUE) {
            completeCommand(request);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void mqttTask(void*) {
    for (;;) {
        mqtt.loop();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

#ifdef WITH_DISPLAY
static void remoteTask(void*) {
    for (;;) {
        CommandRequest request;
        if (xQueueReceive(s_remoteCommands, &request, 0) == pdTRUE) completeCommand(request);
        else pollRemoteUnit();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif

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
    if (!configManager.begin()) Serial.println("ERROR: configuration persistence unavailable");

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

    voltageSensor.begin();
    tuner.setMeterCallback(onMeterUpdate);
    s_commands = xQueueCreate(8, sizeof(CommandRequest));
#ifdef WITH_DISPLAY
    s_remoteCommands = xQueueCreate(8, sizeof(CommandRequest));
#endif
    if (!s_commands || xTaskCreatePinnedToCore(tunerTask, "tuner", 8192, nullptr, 2, nullptr, 1) != pdPASS) {
        Serial.println("ERROR: tuner command task could not start");
        if (s_commands) vQueueDelete(s_commands);
        s_commands = nullptr;
    }

#ifdef WITH_DISPLAY
    Serial.println("Initializing display...");
    display.begin(enqueueDisplayCommand, &meterScale);
#endif

    // Construct MQTT once; its I/O runs independently after network setup.
    static WiFiClient mqttClient;
    mqtt.begin(mqttClient, executeCommand, getActiveState);
    configManager.setPortalIdleCallback([]() {
        // UART is serviced independently, including during network setup.
#ifdef WITH_DISPLAY
        display.updateState(getActiveState());
        display.setCommandPending(s_pendingCommands.load() != 0);
        display.loop();
#endif
    });

    setupWiFi();
    if (xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, nullptr, 1, nullptr, 0) != pdPASS)
        Serial.println("ERROR: MQTT task could not start");
#ifdef WITH_DISPLAY
    if (!s_remoteCommands || xTaskCreatePinnedToCore(remoteTask, "remote", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        Serial.println("ERROR: remote task could not start");
        if (s_remoteCommands) vQueueDelete(s_remoteCommands);
        s_remoteCommands = nullptr;
    }
#endif

#ifdef WITH_DISPLAY
    display.updateNetworkInfo();
#endif

    const bool webStarted = webServer.begin(&tuner, executeCommand, getActiveState,
#ifdef WITH_DISPLAY
                         onRemoteTelemetry,
#else
                         nullptr,
#endif
                         []() { return mqtt.getStatus(); }, &meterScale);
    s_webReady.store(webStarted);
    if (!webStarted) {
        Serial.printf("ERROR: web server failed to start - port %u may still be bound\n", WEB_PORT);
    }

    // HTTP and WebSocket processing never owns UART or LVGL state.
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
    const tuner_state_t state = getActiveState();
    display.updateState(state);
    display.setCommandPending(s_pendingCommands.load() != 0);
    display.loop();
#endif

    delay(1);
}
