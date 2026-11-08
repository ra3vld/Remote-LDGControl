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
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_random.h>
#include "voltage_sensor.h"
#include "remote_protocol.h"
#include "config_manager.h"
#include "tuner_protocol.h"
#include "operating_mode.h"
#include "mqtt_handler.h"
#include "web_server.h"
#include "command_feedback.h"
#include "tci_client.h"
#include "usb_provisioning.h"
#include "build_identity.h"
#include "connection_config.h"
#include <esp_app_desc.h>

#ifdef WITH_DISPLAY
#include "display_ui.h"
#endif

// Global instances
TunerProtocol tuner;
MqttHandler mqtt;
LdgWebServer webServer;
ldg::MeterScale meterScale;
static TciClient radio;
static ldg::RadioState getRadioState() { return radio.state(); }
static std::atomic<bool> s_webReady{false};
static std::atomic<uint32_t> s_webLoopMs{0};
static TaskHandle_t s_radioTask = nullptr;
static TaskHandle_t s_webTask = nullptr;
static TaskHandle_t s_tunerTask = nullptr;
static TaskHandle_t s_mqttTask = nullptr;
static const char s_buildInfo[] = "LDG_METADATA_V1:" LDG_BUILD_METADATA "\n";
static ldg::UsbProvisioning s_usb;
static std::atomic<uint32_t> s_usbUntil{0};
static uint32_t s_usbRestartAt = 0;
static bool usbActive() { const auto until=s_usbUntil.load();return until && int32_t(until - millis()) > 0; }
static void serviceUsb();
#ifdef BOARD_ESP32_3248S035C
SET_LOOP_TASK_STACK_SIZE(7168);
#endif

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
    uint32_t feedbackToken;
    const ldg::BridgeRequest* bridge=nullptr;
    const char** bridgeResult=nullptr;
};
static ldg::CommandFeedback s_commandFeedback;
static QueueHandle_t s_commands = nullptr;
#ifdef WITH_DISPLAY
static QueueHandle_t s_remoteCommands = nullptr;
static TaskHandle_t s_remoteTask = nullptr;
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

// A boot nonce and a target generation identify the physical command recipient.
static uint32_t s_bridgeBoot[3]{},s_bridgeGeneration=0;
static bool s_bridgeRemote=false,s_bridgeIdentified=false;
static char s_bridgeHost[sizeof(DeviceConfig{}.remoteHost)]{},s_bridgeToken[33]{};
static portMUX_TYPE s_bridgeMux=portMUX_INITIALIZER_UNLOCKED;
static ldg::BridgeState getBridgeState() {
    const auto cfg=configManager.get();
    const auto state=getActiveState();
    const bool remote=state.remote;
    ldg::BridgeState context;
    context.remote=remote;
    portENTER_CRITICAL(&s_bridgeMux);
    if (!s_bridgeIdentified || remote!=s_bridgeRemote || strcmp(cfg.remoteHost,s_bridgeHost)) {
        s_bridgeIdentified=true; s_bridgeRemote=remote; strcpy(s_bridgeHost,cfg.remoteHost);
        snprintf(s_bridgeToken,sizeof(s_bridgeToken),"%08lx%08lx%08lx%08lx",
            (unsigned long)s_bridgeBoot[0],(unsigned long)s_bridgeBoot[1],(unsigned long)s_bridgeBoot[2],(unsigned long)++s_bridgeGeneration);
    }
    strcpy(context.sourceToken,s_bridgeToken);
    portEXIT_CRITICAL(&s_bridgeMux);
    context.uptimeMs=millis(); context.meterAgeMs=state.meterAgeMs;
    context.busy=s_pendingCommands.load()!=0;
    context.connected=remote || tuner.isConnected();
    return context;
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
static bool remoteRequest(uint8_t command, JsonDocument& response, const char* pinnedHost=nullptr) {
    const DeviceConfig cfg = configManager.get();
    const char* recipient=pinnedHost ? pinnedHost : cfg.remoteHost;
    if (!recipient[0] || WiFi.status() != WL_CONNECTED) return false;
    WebClient client;
#if TLS_ENABLED
    client.setInsecure();
#endif
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(ldg::remoteRequestTimeout(command));
    http.useHTTP10(true);
    const char* endpoint = command ? ldg::commandEndpoint(command) : "/api/status";
    if (!endpoint || !http.begin(client, String(WEB_SCHEME) + recipient + endpoint)) return false;
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

static bool executeCommandNow(uint8_t cmd, bool remote, const ldg::BridgeRequest* bridge=nullptr) {
#ifdef WITH_DISPLAY
    if (remote) {
        if (!activeRemoteConnected()) return false;
        // Polling shares this worker with commands. Keep its source selected
        // while a long tune runs, but let old meter readings expire normally.
        portENTER_CRITICAL(&s_activeMux);
        s_remoteCommandActive = true;
        portEXIT_CRITICAL(&s_activeMux);
        JsonDocument response;
        const bool result = remoteRequest(cmd, response, bridge ? bridge->targetHost : nullptr);
        pollRemoteUnit();
        portENTER_CRITICAL(&s_activeMux);
        s_remoteCommandActive = false;
        portEXIT_CRITICAL(&s_activeMux);
        mqtt.requestStatePublish();
        return result;
    }
#endif
    bool result = false;
    if (ldg::isModeCommand(cmd)) {
        result = ldg::selectOperatingMode(configManager, tuner, cmd);
        mqtt.requestStatePublish();
        return result;
    }
    switch (cmd) {
        case TUNER_CMD_TOGGLE_ANT:  result = tuner.toggleAntenna(); break;
        case TUNER_CMD_SELECT_ANT_A: result = tuner.selectAntenna(ANT_A); break;
        case TUNER_CMD_SELECT_ANT_B: result = tuner.selectAntenna(ANT_B); break;
        case TUNER_CMD_MEM_TUNE:    result = tuner.memoryTune();    break;
        case TUNER_CMD_FULL_TUNE:   result = tuner.fullTune();      break;
        case TUNER_CMD_BYPASS:      result = tuner.bypass();        break;
    }
    mqtt.requestStatePublish();
    return result;
}

// Synchronous callers wait on their own completion, never on the UI task.
static bool submitCommand(uint8_t command, bool wait, const ldg::BridgeRequest* bridge=nullptr, const char** bridgeResult=nullptr) {
    if (usbActive()) {if (bridgeResult) *bridgeResult="busy";return false;}
    if (bridge) {
        const auto context=getBridgeState();
        const char* error=ldg::validateBridgeRequest(*bridge,context);
        if (!error && context.busy) error="busy";
        if (error) { if (bridgeResult) *bridgeResult=error; return false; }
    }
    const uint32_t feedbackToken = s_commandFeedback.start(command);
    const bool remote = activeRemoteConnected();
    QueueHandle_t destination = s_commands;
#ifdef WITH_DISPLAY
    if (remote) destination = s_remoteCommands;
#endif
    if (!destination) { s_commandFeedback.finish(feedbackToken, false); return false; }
    SemaphoreHandle_t completed = wait ? xSemaphoreCreateBinary() : nullptr;
    if (wait && !completed) { s_commandFeedback.finish(feedbackToken, false); return false; }
    bool result = false;
    CommandRequest request{command, remote, completed, wait ? &result : nullptr, feedbackToken,bridge,bridgeResult};
    if (!remote) tuner.noteManualCommand();
    s_pendingCommands.fetch_add(1);
    if (xQueueSend(destination, &request, 0) != pdTRUE) {
        s_pendingCommands.fetch_sub(1);
        if (completed) vSemaphoreDelete(completed);
        s_commandFeedback.finish(feedbackToken, false);
        return false;
    }
    if (!wait) return true;
    xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return result;
}

bool executeCommand(uint8_t command) { return submitCommand(command, true); }
static const char* executeBridgeCommand(const ldg::BridgeRequest& request) {
    const char* result="failed";
    auto pinned=request;
    const auto cfg=configManager.get();
    static_assert(sizeof(cfg.remoteHost)<=sizeof(pinned.targetHost),"Bridge target host capacity");
    strcpy(pinned.targetHost,cfg.remoteHost);
    submitCommand(pinned.command,true,&pinned,&result);
    return result;
}
#ifdef WITH_DISPLAY
static bool enqueueDisplayCommand(uint8_t command) { return submitCommand(command, false); }
#endif

static void completeCommand(const CommandRequest& request) {
    const char* error=request.bridge ? ldg::validateBridgeRequest(*request.bridge,getBridgeState()) : nullptr;
    if (!error && request.bridge && (activeRemoteConnected()!=request.remote ||
        (request.remote && strcmp(request.bridge->targetHost,configManager.get().remoteHost)))) error="target_changed";
    if (!error && !request.remote && !ldg::isModeCommand(request.command)) tuner.manualCommandStarted();
    const bool result = !error && executeCommandNow(request.command, request.remote, request.bridge);
    if (!error && !request.remote) tuner.manualCommandComplete(request.command, result);
    if (request.bridgeResult) *request.bridgeResult=error ? error : result ? "success" : "failed";
    s_commandFeedback.finish(request.feedbackToken, result);
    if (request.result) *request.result = result;
    s_pendingCommands.fetch_sub(1);
    if (request.completed) xSemaphoreGive(request.completed);
}

static void tunerTask(void*) {
    for (;;) {
        tuner.configureAntennas(configManager.antennaSettings());
        tuner.configureTuning(configManager.tuningSettings());
        tuner.process();
        if (usbActive()) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
        CommandRequest request;
        if (xQueueReceive(s_commands, &request, 0) == pdTRUE) {
            completeCommand(request);
        } else {
            tuner.serviceAntennas(uxQueueMessagesWaiting(s_commands) != 0);
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

static void radioTask(void*) {
    radio.begin();
    for (;;) {
        radio.loop(!activeRemoteConnected());
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

// Disabled transports must not reserve stacks on boards without PSRAM. Only
// the Arduino loop creates these workers; once started they handle live
// configuration changes themselves. Retry allocation failures at most once/s.
static void startConfiguredNetworkTasks() {
    static uint32_t lastAttempt = 0;
    const uint32_t now = millis();
    if (now - lastAttempt < 1000) return;
    lastAttempt = now;
    if (!s_radioTask && configManager.radioSettings().enabled) {
        if (xTaskCreatePinnedToCore(radioTask, "tci", 6144, nullptr, 1, &s_radioTask, 0) != pdPASS)
            Serial.println("ERROR: TCI task could not start");
    }
#ifdef WITH_DISPLAY
    if (!s_remoteTask && s_remoteCommands &&
        (configManager.get().remoteHost[0] || activeRemoteConnected())) {
        if (xTaskCreatePinnedToCore(remoteTask, "remote", 8192, nullptr, 1, &s_remoteTask, 0) != pdPASS)
            Serial.println("ERROR: remote task could not start");
    }
#endif
}

// ============================================================
// Setup
// ============================================================

static void usbRequest(const char* op, JsonVariantConst request, JsonDocument& reply) {
    // Hello grants a short, renewable maintenance lease. A running command
    // finishes normally; saving is rejected until it has completed.
    s_usbUntil.store(millis() + 5000);
    const auto cfg = configManager.get();
    if (!strcmp(op,"hello")) {
        JsonDocument build;deserializeJson(build,s_buildInfo+16);
        for (JsonPairConst pair:build.as<JsonObjectConst>()) reply[pair.key()]=pair.value();
        reply["device_id"]=ldg::mqttIdentity(WiFi.macAddress().c_str()).mac;
        reply["role"]=cfg.remoteHost[0]?"display_proxy":"tuner_controller";
        return;
    }
    if (!strcmp(op,"settings")) {
        auto data=reply["settings"].to<JsonObject>();
        data["wifiSSID"]=cfg.wifiSSID; data["hasWifiPassword"]=bool(cfg.wifiPassword[0]);
        data["mqttBroker"]=cfg.mqttBroker; data["mqttPort"]=cfg.mqttPort;
        data["mqttUsername"]=cfg.mqttUsername; data["hasMqttPassword"]=bool(cfg.mqttPassword[0]);
        data["mqttTls"]=cfg.mqttPort==8883;
        data["mqttDiscoveryEnabled"]=cfg.mqttDiscoveryEnabled;
        data["useStaticIP"]=cfg.useStaticIP;
        data["staticIP"]=cfg.staticIP; data["staticNetmask"]=cfg.staticNetmask;
        data["staticGateway"]=cfg.staticGateway; data["staticDNS"]=cfg.staticDNS;
        data["configured"]=cfg.configured;
        reply["connected"]=WiFi.status()==WL_CONNECTED;
        reply["ip"]=WiFi.localIP().toString();
        reply["mqtt_connected"]=mqtt.isConnected();
        return;
    }
    if (!strcmp(op,"apply") || !strcmp(op,"reboot")) {
        const auto state=getActiveState();
        if (s_pendingCommands.load() || state.radio.transmitting || state.radio.tuning ||
            (state.meterValid && state.meter.forward_power_watts > 0)) {
            reply["status"]=409; reply["error"]="tuner or transmitter busy"; return;
        }
        if (!strcmp(op,"reboot")) {s_usbRestartAt=millis()+500;reply["restart"]=true;return;}
        const auto patch=request["settings"];
        static const char* allowed[]={"wifiSSID","wifiPassword","clearWifiPassword","mqttBroker","mqttPort",
            "mqttUsername","mqttPassword","clearMqttPassword","mqttDiscoveryEnabled","useStaticIP",
            "staticIP","staticNetmask","staticGateway","staticDNS"};
        if (!patch.is<JsonObjectConst>()) {reply["status"]=400;reply["error"]="settings object required";return;}
        for (JsonPairConst pair:patch.as<JsonObjectConst>()) {
            bool known=false;for (auto key:allowed) if (!strcmp(pair.key().c_str(),key)) known=true;
            if (!known) {reply["status"]=400;reply["error"]="unsupported setting";return;}
        }
        auto candidate=cfg;
        if (!ldg::patchConnectionConfig(patch,candidate,true)) {reply["status"]=400;reply["error"]="invalid settings";return;}
        if (!configManager.update(candidate)) {reply["status"]=500;reply["error"]="settings save failed";return;}
        reply["saved"]=true;reply["restart"]=ldg::networkSettingsChanged(cfg,candidate);
        if (ldg::mqttSettingsChanged(cfg,candidate)) mqtt.requestReconfigure();
        return;
    }
    reply["status"]=404;reply["error"]="unknown USB operation";
}

static void serviceUsb() {
    static char input[ldg::UsbProvisioning::Limit+1],output[ldg::UsbProvisioning::Limit+2];
    static size_t used=0;
    static bool overflow=false;
    for (unsigned count=0;count<128 && Serial.available();++count) {
        const char c=char(Serial.read());
        if (c=='\n') {
            input[used]=0;
            if (!overflow && used && s_usb.handle(input,output,sizeof(output)-1)) {
                const size_t size=strlen(output);output[size]='\n';Serial.write(reinterpret_cast<uint8_t*>(output),size+1);
            }
            used=0;overflow=false;
        } else if (c!='\r') {
            if (used<ldg::UsbProvisioning::Limit && !overflow) input[used++]=c;
            else overflow=true;
        }
    }
    if (s_usbRestartAt && int32_t(millis()-s_usbRestartAt)>=0) {Serial.flush();ESP.restart();}
}

void setup() {
    esp_fill_random(s_bridgeBoot,sizeof(s_bridgeBoot));
    Serial.begin(115200);
    char usbBoot[17];snprintf(usbBoot,sizeof(usbBoot),"%08lx%08lx",(unsigned long)s_bridgeBoot[0],(unsigned long)s_bridgeBoot[1]);
    s_usb.begin(usbBoot,usbRequest);
    heap_caps_register_failed_alloc_callback([](size_t bytes, uint32_t caps, const char* operation) {
        static std::atomic<uint32_t> lastReport{0};
        const uint32_t now = millis();
        if (now - lastReport.load() < 1000) return;
        lastReport.store(now);
        // ROM output avoids an allocation in the failure path itself.
        esp_rom_printf("[HEAP] failed bytes=%u caps=%x free=%u largest=%u op=%s\n",
                       unsigned(bytes), unsigned(caps),
                       unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                       unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), operation);
    });
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
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("[WiFi] Station disconnected: reason=%u\n",
                      unsigned(info.wifi_sta_disconnected.reason));
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    // Load configuration from NVS
    if (!configManager.begin()) Serial.println("ERROR: configuration persistence unavailable");
    const auto savedMeter = configManager.meterSettings();
    tuner.setRadioStateGetter(getRadioState);
    meterScale.set(savedMeter.watts, savedMeter.logarithmic);

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
    // The display profile has a measured bounded command worker; other
    // profiles retain their existing stack allocation.
#ifdef BOARD_ESP32_3248S035C
    constexpr unsigned tunerStack = 6144;
#else
    constexpr unsigned tunerStack = 8192;
#endif
    if (!s_commands || xTaskCreatePinnedToCore(tunerTask, "tuner", tunerStack, nullptr, 2, &s_tunerTask, 1) != pdPASS) {
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
    mqtt.begin(mqttClient, executeCommand, getActiveState, getBridgeState, executeBridgeCommand);
    configManager.setPortalIdleCallback([]() {
        serviceUsb();
        // UART is serviced independently, including during network setup.
#ifdef WITH_DISPLAY
        display.updateState(getActiveState());
        display.setCommandPending(s_pendingCommands.load() != 0);
        display.updateCommandFeedback(s_commandFeedback.snapshot());
        display.loop();
#endif
    });

    setupWiFi();
#ifdef BOARD_ESP32_3248S035C
    // A continuously connected panel favours prompt TCP ACKs over modem sleep.
    // Avoid retaining several HTTP/WS frames through each DTIM sleep interval.
    WiFi.setSleep(false);
#endif
    // Share CPU0 with web/TCI instead of waiting behind every runnable network
    // task. TLS yields after releasing the MPI accelerator so IDLE0 can still
    // service its watchdog; UART metering keeps priority 2 on CPU1.
    if (xTaskCreatePinnedToCore(mqttTask, "mqtt", 7168, nullptr, 1, &s_mqttTask, 0) != pdPASS)
        Serial.println("ERROR: MQTT task could not start");

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
    webServer.setMqttReconfigureHandler([]() { mqtt.requestReconfigure(); });
    if (!webStarted) {
        Serial.printf("ERROR: web server failed to start - port %u may still be bound\n", WEB_PORT);
    }

    // Keep network work on core 0 with MQTT/remote polling. A high-priority
    // web task on core 1 can preempt LVGL for an entire socket/logging burst.
    // UART metering retains priority 2 on core 1; UI stays on the Arduino loop.
    if (xTaskCreatePinnedToCore(
        [](void*) {
            for (;;) {
                webServer.loop();
                s_webLoopMs.store(millis());
                // Socket polling every millisecond consumes CPU even with no
                // clients. Leave time for the Wi-Fi/TCP workers and MQTT.
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        },
        "webServer", TLS_ENABLED ? 16384 : 8704, NULL, 1, &s_webTask, 0) != pdPASS)
        Serial.println("ERROR: web task could not start");

    startConfiguredNetworkTasks();

    Serial.println("=== System Ready ===");
}

// ============================================================
// Loop
// ============================================================

void loop() {
    serviceUsb();
    startConfiguredNetworkTasks();
    // WiFi reconnection — only manage STA when STA is actually configured.
    // In AP-only mode (captive portal still running, never configured) there
    // is no STA to reconnect, and WiFi.status() returns disconnected forever.
    if (millis() - lastWiFiCheck > 10000) {
        lastWiFiCheck = millis();
        wifi_mode_t mode = WiFi.getMode();
        bool staActive = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
        Serial.printf("[NET] wifi_status=%d web_loop_age_ms=%lu heap=%u min_heap=%u largest=%u\n", int(WiFi.status()),
                      (unsigned long)(millis() - s_webLoopMs.load()),
                      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        Serial.printf("[STACK] loop=%u web=%u tuner=%u mqtt=%u tci=%u remote=%u\n",
                      unsigned(uxTaskGetStackHighWaterMark(nullptr)),
                      unsigned(s_webTask ? uxTaskGetStackHighWaterMark(s_webTask) : 0),
                      unsigned(s_tunerTask ? uxTaskGetStackHighWaterMark(s_tunerTask) : 0),
                      unsigned(s_mqttTask ? uxTaskGetStackHighWaterMark(s_mqttTask) : 0),
                      unsigned(s_radioTask ? uxTaskGetStackHighWaterMark(s_radioTask) : 0),
#ifdef WITH_DISPLAY
                      unsigned(s_remoteTask ? uxTaskGetStackHighWaterMark(s_remoteTask) : 0)
#else
                      0u
#endif
        );
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
    display.updateCommandFeedback(s_commandFeedback.snapshot());
    display.loop();
#endif

    delay(1);
}
