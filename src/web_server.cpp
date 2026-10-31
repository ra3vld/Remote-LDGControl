#include "web_server.h"
#include "config_manager.h"
#include "config.h"
#include "mqtt_protocol.h"
#include "security.h"
#include "config_store.h"
#include "remote_protocol.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <HTTPServer.hpp>
#if TLS_ENABLED
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include "portal_cert.h"
#endif
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <WebsocketHandler.hpp>
#include <WebsocketNode.hpp>
#include <freertos/FreeRTOS.h>
#include <vector>
#include <algorithm>

namespace hsv = httpsserver;

// ---------------------------------------------------------------------------
// Singleton + shared meter snapshots.
//
// The HTTP library accepts plain function pointers for route handlers, so we
// can't capture `this` in lambdas. Handlers read from s_instance to reach
// LdgWebServer members. Producers replace the shared snapshot under a short
// critical section; only the web task touches WebSocket subscribers.
// ---------------------------------------------------------------------------

static LdgWebServer* s_instance = nullptr;

struct MeterFeed {
    portMUX_TYPE mux;
    uint32_t meterSeq;
    char meterJson[384];
};
static MeterFeed s_feed = {
    portMUX_INITIALIZER_UNLOCKED,
    0, ""
};

// ---------------------------------------------------------------------------
// WebSocket meter subscribers.
//
// Unlike SSE, the library dispatches WS handlers non-inline — the slot stays
// in STATE_WEBSOCKET and the connection's loop() polls for client data, so a
// long-lived WS handler doesn't block the slot the way h_events does. That
// lets us keep one persistent connection per browser (no SSE session caps
// or close-on-button-click dance) AND let
// command POSTs dispatch on free slots concurrently.
//
// Registry lives in the webServer task only — both registration (handler
// ctor) and broadcast (LdgWebServer::loop) execute there sequentially, so we
// don't need a mutex around the vector. The handler's dtor unregisters
// itself; the library calls delete on the handler from STATE_WEBSOCKET when
// the client closes (or the connection errors), and that happens between
// HTTPServer::loop() iterations, never concurrently with broadcastMeter().
// ---------------------------------------------------------------------------
class MeterWebsocketHandler : public hsv::WebsocketHandler {
public:
    MeterWebsocketHandler() { s_subs.push_back(this); }
    ~MeterWebsocketHandler() override {
        s_subs.erase(std::remove(s_subs.begin(), s_subs.end(), this), s_subs.end());
    }

    // Client → server messages are ignored for now; commands still use the
    // existing POST /api/command/* endpoints.
    void onMessage(hsv::WebsocketInputStreambuf* buf) override { (void)buf; }
    void onClose() override {}
    void onError(std::string err) override { (void)err; }

    // Factory for hsv::WebsocketNode.
    static hsv::WebsocketHandler* create() { return new MeterWebsocketHandler(); }

    // Push a meter JSON to every active subscriber. Single-threaded by design
    // — called from the webServer task only, never concurrent with
    // HTTPServer::loop()'s slot management.
    static void broadcastMeter(const char* json) {
        for (auto* h : s_subs) {
            if (!h->closed()) {
                h->send(std::string(json), SEND_TYPE_TEXT);
            }
        }
    }

private:
    static std::vector<MeterWebsocketHandler*> s_subs;
};

std::vector<MeterWebsocketHandler*> MeterWebsocketHandler::s_subs;

// Middleware: Basic-Auth-gate the WebSocket upgrade. The library's
// handleWebsocketHandshake doesn't authenticate, but middleware runs ahead
// of it in the chain — if we send 401 and skip next(), the handshake never
// happens and the slot closes cleanly. Browsers attach cached Basic Auth
// credentials to the WS upgrade request on the same origin, so this is
// transparent to the user once they've logged in via the regular page.
// Short-lived bearer tokens for the WebSocket upgrade.
//
// Browsers don't reliably send `Authorization: Basic ...` on WebSocket
// handshake requests (Chrome strips it, Firefox passes it, behavior varies
// by version). Basic Auth alone can't gate /api/ws. Pattern: the page (which
// IS Basic-Auth-gated) fetches a token from /api/ws-token, then opens the
// WS connection with ?t=<token> in the URL. Middleware validates the token
// out of the query string.
struct WsToken {
    char     value[33];    // 32 hex chars + null
    uint32_t expiresAtMs;  // millis() value; 0 = empty slot
};
static const int      WS_TOKEN_POOL_SIZE = 4;
static const uint32_t WS_TOKEN_TTL_MS    = 60 * 1000;  // 1 min, consumed on first connect
static WsToken s_wsTokens[WS_TOKEN_POOL_SIZE] = {};

static String issueWsToken() {
    uint8_t bin[16];
    esp_fill_random(bin, sizeof(bin));
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(&hex[i*2], 3, "%02x", bin[i]);
    hex[32] = '\0';

    // Reuse the slot with the soonest expiry (empty slots have expiresAtMs == 0
    // and therefore win the tiebreak).
    int slot = 0;
    for (int i = 1; i < WS_TOKEN_POOL_SIZE; i++) {
        if (!s_wsTokens[i].value[0] || int32_t(s_wsTokens[i].expiresAtMs - s_wsTokens[slot].expiresAtMs) < 0) slot = i;
    }
    strncpy(s_wsTokens[slot].value, hex, sizeof(s_wsTokens[slot].value));
    s_wsTokens[slot].expiresAtMs = millis() + WS_TOKEN_TTL_MS;
    return String(hex);
}

// Consume the token — returns true if found and unexpired; clears the slot.
static bool consumeWsToken(const std::string& token) {
    uint32_t now = millis();
    for (int i = 0; i < WS_TOKEN_POOL_SIZE; i++) {
        if (s_wsTokens[i].value[0] && int32_t(s_wsTokens[i].expiresAtMs - now) > 0 &&
            token == s_wsTokens[i].value) {
            s_wsTokens[i].expiresAtMs = 0;
            s_wsTokens[i].value[0] = '\0';
            return true;
        }
    }
    return false;
}

static void wsAuthMiddleware(hsv::HTTPRequest* req, hsv::HTTPResponse* res,
                             std::function<void()> next) {
    const std::string url = req->getRequestString();
    // URL is path[?query]. Prefix-match to find /api/ws with or without a
    // query string, but not e.g. /api/ws-token (different endpoint).
    if (url.compare(0, 7, "/api/ws") == 0 &&
        (url.length() == 7 || url[7] == '?')) {
        size_t qpos = url.find("?t=");
        std::string token = (qpos == std::string::npos) ? "" : url.substr(qpos + 3);
        size_t amp = token.find('&');
        if (amp != std::string::npos) token = token.substr(0, amp);
        Serial.printf("WS-MW: url='%s' token='%.8s...' len=%u\n",
                      url.c_str(), token.c_str(), (unsigned)token.length());
        if (token.empty() || !consumeWsToken(token)) {
            Serial.println("WS-MW: token reject → 401");
            res->setStatusCode(401);
            res->setHeader("Content-Type", "text/plain");
            res->print("WS token missing or invalid");
            return;  // don't call next() → no WS handshake
        }
        Serial.println("WS-MW: token ok → next()");
    }
    next();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool authenticate(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    std::string user = req->getBasicAuthUser();
    std::string pass = req->getBasicAuthPassword();
    const auto authorization = Security::authorize(req->getClientIP(), String(user.c_str()), String(pass.c_str()));
    if (authorization == Security::Authorization::Allowed) return true;
    if (authorization == Security::Authorization::RateLimited || authorization == Security::Authorization::Locked) {
        res->setStatusCode(429);
        res->setHeader("Retry-After", authorization == Security::Authorization::Locked ? "300" : "1");
        res->print("Too many requests or login attempts");
        return false;
    }
    res->setStatusCode(401);
    res->setHeader("WWW-Authenticate", "Basic realm=\"LDG Tuner\"");
    res->setHeader("Content-Type", "text/plain");
    res->print("Unauthorized");
    return false;
}

static std::string readRequestBody(hsv::HTTPRequest* req) {
    // readBytes() is non-blocking and returns 0 between TCP packets, not only
    // at end-of-body. Drive the loop off requestComplete() so we don't bail
    // mid-body when the receive buffer is briefly empty.
    std::string body;
    uint8_t buf[256];
    uint32_t deadline = millis() + 5000;
    while (!req->requestComplete() && millis() < deadline) {
        size_t n = req->readBytes(buf, sizeof(buf));
        if (n > 0) {
            body.append((char*)buf, n);
            if (body.size() > 8192) break;  // sanity cap
        } else {
            delay(1);
        }
    }
    return body;
}

static void sendJson(hsv::HTTPResponse* res, int code, const String& body) {
    res->setStatusCode(code);
    res->setHeader("Content-Type", "application/json");
    res->print(body.c_str());
}

// Web transports retain numeric enums; MQTT uses text labels from the same snapshot.
static String serializeState(const tuner_state_t& state) {
    JsonDocument doc;
    ldg::writeState(doc, state, false);
    if (s_instance && s_instance->getMeterScale())
        ldg::writeMeterScale(doc, s_instance->getMeterScale()->snapshot());
    String out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------------------
// Handlers (all plain functions; access state through s_instance / globals)
// ---------------------------------------------------------------------------

static void h_root(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;
    res->setHeader("Content-Type", "text/html; charset=utf-8");

    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        if (f) {
            uint8_t buf[512];
            int n;
            while ((n = f.read(buf, sizeof(buf))) > 0) {
                res->write(buf, n);
            }
            f.close();
            return;
        }
    }

    res->print(
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>LDG Tuner</title>"
        "</head><body><h1>LDG Tuner</h1>"
        "<p>The web UI file (/index.html) is missing from LittleFS. "
        "Re-upload the data partition.</p>"
        "</body></html>"
    );
}

static void h_status(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;

    LdgWebServer* self = s_instance;
    if (!self) { sendJson(res, 500, "{\"error\":\"no instance\"}"); return; }

    const tuner_state_t state = self->getStateGetter() ? self->getStateGetter()() :
        self->getTuner() ? self->getTuner()->getState() : tuner_state_t{};
    JsonDocument doc;
    ldg::writeState(doc, state, false);
    if (self->getMeterScale())
        ldg::writeMeterScale(doc, self->getMeterScale()->snapshot());
    doc["connected"] = state.remote || (self->getTuner() && self->getTuner()->isConnected());
    doc["uptime_ms"] = millis();
    doc["reset_reason"] = int(esp_reset_reason());
    doc["firmware_build"] = __DATE__ " " __TIME__;
    if (self->getTuner()) {
        ldg::writeTunerDiagnostics(doc["uart"].to<JsonObject>(),
            self->getTuner()->getDiagnostics(), millis());
    }
    const ldg::MqttStatus mqttStatus = self->getMqttStatusGetter()
        ? self->getMqttStatusGetter()() : ldg::MqttStatus{};
    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    ldg::writeMqttStatus(mqtt, mqttStatus, millis());
    const DeviceConfig& cfg = configManager.get();
    mqtt["broker"] = cfg.mqttBroker;
    mqtt["port"] = cfg.mqttPort;
    String out;
    serializeJson(doc, out);
    sendJson(res, 200, out);
}

static void h_meter_scale(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;
    ldg::MeterScale* scale = s_instance ? s_instance->getMeterScale() : nullptr;
    if (!scale) { sendJson(res, 503, "{\"error\":\"meter scale unavailable\"}"); return; }

    JsonDocument request;
    if (deserializeJson(request, readRequestBody(req)) ||
        !ldg::applyMeterScaleRequest(*scale, request.as<JsonVariantConst>())) {
        sendJson(res, 400, "{\"error\":\"use meter_scale: 100 or 1000, or toggle: true\"}");
        return;
    }
    JsonDocument response;
    ldg::writeMeterScale(response, scale->snapshot());
    String out;
    serializeJson(response, out);
    sendJson(res, 200, out);
}

static void h_mqtt_reconnect(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;
    if (!s_instance || !s_instance->requestMqttReconfigure()) {
        sendJson(res, 503, "{\"error\":\"MQTT worker unavailable\"}"); return;
    }
    sendJson(res, 200, "{\"result\":\"ok\"}");
}

static void h_config_get(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;

    const DeviceConfig& cfg = configManager.get();
    JsonDocument doc;
    doc["mqttBroker"]      = cfg.mqttBroker;
    doc["mqttPort"]        = cfg.mqttPort;
    doc["mqttUsername"]    = cfg.mqttUsername;
    doc["mqttDiscoveryEnabled"] = cfg.mqttDiscoveryEnabled;
    doc["mqttBaseTopic"] = ldg::mqttIdentity(WiFi.macAddress().c_str()).base;
    doc["webUsername"]     = cfg.webUsername;
    doc["meterPsuVoltage"] = cfg.meterPsuVoltage;
    doc["ant1Name"]        = cfg.ant1Name;
    doc["ant2Name"]        = cfg.ant2Name;
    doc["configured"]      = cfg.configured;
    doc["useStaticIP"]     = cfg.useStaticIP;
    doc["staticIP"]        = cfg.staticIP;
    doc["staticNetmask"]   = cfg.staticNetmask;
    doc["staticGateway"]   = cfg.staticGateway;
    doc["staticDNS"]       = cfg.staticDNS;
    doc["currentIP"]       = WiFi.localIP().toString();
    doc["currentNetmask"]  = WiFi.subnetMask().toString();
    doc["currentGateway"]  = WiFi.gatewayIP().toString();
    doc["currentDNS"]      = WiFi.dnsIP().toString();
    doc["wifiSSID"]        = WiFi.SSID();
#ifdef WITH_DISPLAY
    doc["remoteHost"]      = cfg.remoteHost;
    doc["build"]           = "display";
#else
    doc["build"]           = "remote";
#endif
    String out;
    serializeJson(doc, out);
    sendJson(res, 200, out);
}

static void h_config_post(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;

    std::string body = readRequestBody(req);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.is<JsonObject>()) { sendJson(res, 400, "{\"error\":\"invalid json object\"}"); return; }

    if (doc["reset"].is<bool>() && doc["reset"].as<bool>()) {
        if (!configManager.reset()) { sendJson(res, 500, "{\"error\":\"reset failed\"}"); return; }
        sendJson(res, 200, "{\"result\":\"ok\",\"restart\":true}");
        delay(500);
        ESP.restart();
        return;
    }

    const DeviceConfig before = configManager.get();
    DeviceConfig cfg = before;
    if (doc["mqttBroker"].is<const char*>())
        strlcpy(cfg.mqttBroker, doc["mqttBroker"], sizeof(cfg.mqttBroker));
    if (!doc["mqttPort"].isUnbound()) {
        if (!doc["mqttPort"].is<uint16_t>() || doc["mqttPort"].as<uint16_t>() == 0) {
            sendJson(res, 400, "{\"error\":\"invalid MQTT port\"}"); return;
        }
        cfg.mqttPort = doc["mqttPort"];
    }
    if (doc["mqttUsername"].is<const char*>())
        strlcpy(cfg.mqttUsername, doc["mqttUsername"], sizeof(cfg.mqttUsername));
    // Passwords are never returned by GET; an empty form field preserves the secret.
    if (doc["mqttPassword"].is<const char*>() && doc["mqttPassword"].as<const char*>()[0])
        strlcpy(cfg.mqttPassword, doc["mqttPassword"], sizeof(cfg.mqttPassword));
    if (doc["mqttDiscoveryEnabled"].is<bool>())
        cfg.mqttDiscoveryEnabled = doc["mqttDiscoveryEnabled"].as<bool>();
    if (doc["webUsername"].is<const char*>()) strlcpy(cfg.webUsername, doc["webUsername"], sizeof(cfg.webUsername));
    if (doc["webPassword"].is<const char*>() && doc["webPassword"].as<const char*>()[0])
        strlcpy(cfg.webPassword, doc["webPassword"], sizeof(cfg.webPassword));
    if (!doc["meterPsuVoltage"].isUnbound()) {
        if (!doc["meterPsuVoltage"].is<float>()) { sendJson(res, 400, "{\"error\":\"invalid voltage\"}"); return; }
        cfg.meterPsuVoltage = doc["meterPsuVoltage"];
    }
    if (doc["ant1Name"].is<const char*>()) strlcpy(cfg.ant1Name, doc["ant1Name"], sizeof(cfg.ant1Name));
    if (doc["ant2Name"].is<const char*>()) strlcpy(cfg.ant2Name, doc["ant2Name"], sizeof(cfg.ant2Name));
    if (doc["useStaticIP"].is<bool>())  cfg.useStaticIP = doc["useStaticIP"];
    if (doc["staticIP"].is<const char*>()) strlcpy(cfg.staticIP, doc["staticIP"], sizeof(cfg.staticIP));
    if (doc["staticNetmask"].is<const char*>()) strlcpy(cfg.staticNetmask, doc["staticNetmask"], sizeof(cfg.staticNetmask));
    if (doc["staticGateway"].is<const char*>()) strlcpy(cfg.staticGateway, doc["staticGateway"], sizeof(cfg.staticGateway));
    if (doc["staticDNS"].is<const char*>()) strlcpy(cfg.staticDNS, doc["staticDNS"], sizeof(cfg.staticDNS));
#ifdef WITH_DISPLAY
    if (doc["remoteHost"].is<const char*>()) strlcpy(cfg.remoteHost, doc["remoteHost"], sizeof(cfg.remoteHost));
#endif
    cfg.configured = true;

    if (!ldg::validDeviceConfig(cfg)) { sendJson(res, 400, "{\"error\":\"invalid configuration\"}"); return; }
    if (configManager.update(cfg)) {
        const bool restart = ldg::networkSettingsChanged(before, cfg);
        if (ldg::mqttSettingsChanged(before, cfg) && s_instance)
            s_instance->requestMqttReconfigure();
        sendJson(res, 200, restart ? "{\"result\":\"ok\",\"restart\":true}"
                                  : "{\"result\":\"ok\",\"restart\":false}");
        if (restart) { delay(500); ESP.restart(); }
    } else {
        sendJson(res, 500, "{\"error\":\"save failed\"}");
    }
}

// Each command endpoint as a separate handler — the library's route registration
// takes a function pointer, so we can't bind the cmd parameter.
static void runCommand(hsv::HTTPResponse* res, uint8_t cmd) {
    LdgWebServer* self = s_instance;
    if (!self) { sendJson(res, 500, "{\"error\":\"no instance\"}"); return; }

    bool ok = false;
    if (self->getCmdHandler()) {
        ok = self->getCmdHandler()(cmd);
    } else if (self->getTuner()) {
        switch (cmd) {
            case TUNER_CMD_SELECT_ANT_A: ok = self->getTuner()->selectAntenna(ANT_A); break;
            case TUNER_CMD_SELECT_ANT_B: ok = self->getTuner()->selectAntenna(ANT_B); break;
            case TUNER_CMD_TOGGLE_ANT:  ok = self->getTuner()->toggleAntenna();   break;
            case TUNER_CMD_MEM_TUNE:    ok = self->getTuner()->memoryTune();      break;
            case TUNER_CMD_FULL_TUNE:   ok = self->getTuner()->fullTune();        break;
            case TUNER_CMD_BYPASS:      ok = self->getTuner()->bypass();          break;
            case TUNER_CMD_AUTO_MODE:   ok = self->getTuner()->setAutoMode();     break;
            case TUNER_CMD_MANUAL_MODE: ok = self->getTuner()->setManualMode();   break;
        }
    }
    JsonDocument doc;
    doc["command"] = cmd;
    doc["result"]  = ok ? "success" : "failed";
    String out;
    serializeJson(doc, out);
    sendJson(res, 200, out);
}

// Issue a single-use WS upgrade token for the current authenticated user.
static void h_ws_token(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;
    String body = "{\"token\":\"" + issueWsToken() + "\"}";
    sendJson(res, 200, body);
}

static void h_cmd_antenna_1(hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (authenticate(req, res)) runCommand(res, TUNER_CMD_SELECT_ANT_A); }
static void h_cmd_antenna_2(hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (authenticate(req, res)) runCommand(res, TUNER_CMD_SELECT_ANT_B); }
static void h_cmd_toggle      (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_TOGGLE_ANT); }
static void h_cmd_memory_tune (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_MEM_TUNE); }
static void h_cmd_full_tune   (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_FULL_TUNE); }
static void h_cmd_bypass      (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_BYPASS); }
static void h_cmd_auto        (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_AUTO_MODE); }
static void h_cmd_manual      (hsv::HTTPRequest* req, hsv::HTTPResponse* res) { if (!authenticate(req, res)) return; runCommand(res, TUNER_CMD_MANUAL_MODE); }

static void h_telemetry(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;

    std::string body = readRequestBody(req);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.is<JsonObject>()) { sendJson(res, 400, "{\"error\":\"invalid json object\"}"); return; }

    if (!ldg::validRemoteStatus(doc.as<JsonVariantConst>())) {
        sendJson(res, 400, "{\"error\":\"invalid telemetry snapshot\"}"); return;
    }
    tuner_state_t state = ldg::readWebState(doc.as<JsonVariantConst>());
    state.remote = true;
    if (s_instance) {
        if (s_instance->getRemoteTelemetryHandler()) {
            s_instance->getRemoteTelemetryHandler()(state);
        } else {
            s_instance->pushMeterEvent(state);
        }
    }
    sendJson(res, 200, "{\"result\":\"ok\"}");
}


// ---------------------------------------------------------------------------
// LdgWebServer methods
// ---------------------------------------------------------------------------

LdgWebServer::LdgWebServer()
    : m_server(nullptr),
#if TLS_ENABLED
      m_sslCert(nullptr),
#endif
      m_tuner(nullptr),
      m_cmdHandler(nullptr), m_stateGetter(nullptr),
      m_remoteTelemetryHandler(nullptr), m_mqttStatusGetter(nullptr),
      m_meterScale(nullptr), m_lastMeterUpdate(0) {
    s_instance = this;
}

LdgWebServer::~LdgWebServer() {
    if (m_server) { m_server->stop(); delete m_server; }
#if TLS_ENABLED
    delete m_sslCert;
#endif
    if (s_instance == this) s_instance = nullptr;
}

bool LdgWebServer::begin(TunerProtocol* tuner, command_handler_t cmdHandler,
                         state_getter_t stateGetter,
                         remote_telemetry_handler_t remoteTelemetryHandler,
                         mqtt_status_getter_t mqttStatusGetter,
                         ldg::MeterScale* meterScale) {
    Security::begin();
    m_tuner = tuner;
    m_cmdHandler = cmdHandler;
    m_stateGetter = stateGetter;
    m_remoteTelemetryHandler = remoteTelemetryHandler;
    m_mqttStatusGetter = mqttStatusGetter;
    m_meterScale = meterScale;
    s_instance = this;

    // setupWiFi() reboots after portal setup, leaving WEB_PORT free here.
    // Bind to every interface, including the display unit's softAP.
#if TLS_ENABLED
    m_sslCert = portalCert_newSSLCert();
    if (!m_sslCert) {
        Serial.println("ERROR: runtime web server: no TLS certificate");
        return false;
    }
    // Leave room for a persistent WebSocket and one command connection.
    m_server = new hsv::HTTPSServer(m_sslCert, WEB_PORT, 2);
#else
    m_server = new hsv::HTTPServer(HTTP_PORT, 4);
#endif

    m_server->registerNode(new hsv::ResourceNode("/",                   "GET",  &h_root));
    m_server->registerNode(new hsv::ResourceNode("/api/status",         "GET",  &h_status));
    m_server->registerNode(new hsv::ResourceNode("/api/meter-scale",    "POST", &h_meter_scale));
    m_server->registerNode(new hsv::ResourceNode("/api/mqtt/reconnect", "POST", &h_mqtt_reconnect));
    m_server->registerNode(new hsv::ResourceNode("/api/config",         "GET",  &h_config_get));
    m_server->registerNode(new hsv::ResourceNode("/api/config",         "POST", &h_config_post));
    m_server->registerNode(new hsv::ResourceNode("/api/telemetry",      "POST", &h_telemetry));
    m_server->registerNode(new hsv::ResourceNode("/api/command/antenna_1", "POST", &h_cmd_antenna_1));
    m_server->registerNode(new hsv::ResourceNode("/api/command/antenna_2", "POST", &h_cmd_antenna_2));
    m_server->registerNode(new hsv::ResourceNode("/api/command/toggle",      "POST", &h_cmd_toggle));
    m_server->registerNode(new hsv::ResourceNode("/api/command/memory_tune", "POST", &h_cmd_memory_tune));
    m_server->registerNode(new hsv::ResourceNode("/api/command/full_tune",   "POST", &h_cmd_full_tune));
    m_server->registerNode(new hsv::ResourceNode("/api/command/bypass",      "POST", &h_cmd_bypass));
    m_server->registerNode(new hsv::ResourceNode("/api/command/auto",        "POST", &h_cmd_auto));
    m_server->registerNode(new hsv::ResourceNode("/api/command/manual",      "POST", &h_cmd_manual));

    // WebSocket telemetry channel — persistent, non-blocking, replaces SSE
    // for the live meter feed. The library dispatches WS handlers non-inline,
    // so a connected browser does not block command POSTs the way SSE did.
    // /api/ws-token issues a single-use bearer token (Basic Auth required);
    // the page fetches one and opens the WS at /api/ws?t=<token>. Browsers
    // don't reliably send Basic Auth on WS upgrades, hence the token dance.
    m_server->registerNode(new hsv::ResourceNode("/api/ws-token",            "GET",  &h_ws_token));
    m_server->addMiddleware(&wsAuthMiddleware);
    m_server->registerNode(new hsv::WebsocketNode("/api/ws", &MeterWebsocketHandler::create));

    m_server->start();
    if (!m_server->isRunning()) {
        Serial.printf("ERROR: runtime web server failed to bind :%u\n", WEB_PORT);
        return false;
    }
    Serial.printf("Runtime web server started: %s%s/\n",
                  WEB_SCHEME, WiFi.localIP().toString().c_str());
    return true;
}

void LdgWebServer::loop() {
    if (m_server) m_server->loop();

    // Publish fresh validity and shared scale changes even when UART is idle.
    const uint32_t scaleRevision = m_meterScale ? m_meterScale->snapshot().revision : 0;
    if (millis() - m_lastMeterUpdate > 500 || scaleRevision != m_lastMeterScaleRevision) {
        if (m_stateGetter) pushMeterEvent(m_stateGetter());
        else if (m_tuner) pushMeterEvent(m_tuner->getState());
        m_lastMeterUpdate = millis();
        m_lastMeterScaleRevision = scaleRevision;
    }

    // Broadcast newly-published meter data to every connected WebSocket
    // subscriber. seq-based gating means we only send when something actually
    // changed since the last broadcast. This runs in the webServer task right
    // after HTTPServer::loop(), so it cannot race with the library's
    // creation/deletion of MeterWebsocketHandler instances.
    uint32_t currSeq;
    char buf[sizeof(s_feed.meterJson)];
    portENTER_CRITICAL(&s_feed.mux);
    currSeq = s_feed.meterSeq;
    if (currSeq != m_lastWsMeterSeq) {
        strncpy(buf, s_feed.meterJson, sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_feed.mux);
    if (currSeq != m_lastWsMeterSeq) {
        MeterWebsocketHandler::broadcastMeter(buf);
        m_lastWsMeterSeq = currSeq;
    }
}

void LdgWebServer::pushMeterEvent(const tuner_state_t& state) {
    String payload = serializeState(state);
    portENTER_CRITICAL(&s_feed.mux);
    strncpy(s_feed.meterJson, payload.c_str(), sizeof(s_feed.meterJson));
    s_feed.meterJson[sizeof(s_feed.meterJson) - 1] = '\0';
    s_feed.meterSeq++;
    portEXIT_CRITICAL(&s_feed.mux);
}

