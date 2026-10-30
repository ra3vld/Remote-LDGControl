#include "config_manager.h"
#include <DNSServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPServer.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <esp_heap_caps.h>
#include "config_store.h"
#if TLS_ENABLED
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include "portal_cert.h"
#endif

ConfigManager configManager;

static DNSServer* dnsServer = nullptr;
static const uint32_t PORTAL_HEAP_LOG_INTERVAL_MS = 30000;

static httpsserver::HTTPServer* portalServer = nullptr;
#if TLS_ENABLED
static httpsserver::HTTPServer* portalRedirectServer = nullptr;
static httpsserver::SSLCert* sslCert = nullptr;
#endif

static void handlePortalRedirect(httpsserver::HTTPRequest* req,
                                 httpsserver::HTTPResponse* res) {
    res->setHeader("Cache-Control", "no-store");
    res->setHeader("Connection", "close");
    if (req->getMethod() != "GET" && req->getMethod() != "HEAD") {
        res->setStatusCode(405);
        res->setStatusText("Method Not Allowed");
        res->setHeader("Allow", "GET, HEAD");
        return;
    }

    // Use our AP address, never the probe's Host/path (e.g. /generate_204).
    const String url = String(WEB_SCHEME) + WiFi.softAPIP().toString() + "/";
    res->setStatusCode(302);
    res->setStatusText("Found");
    res->setHeader("Location", url.c_str());
    res->setHeader("Content-Type", "text/plain; charset=utf-8");
    if (req->getMethod() == "GET") {
        res->print("Open setup in your browser: ");
        res->print(url);
        res->print("\n");
    }
}

// ResourceResolver does not own its nodes; keep the fallback alive statically.
static httpsserver::ResourceNode portalRedirectNode("", "GET", &handlePortalRedirect);

static void portalResponseMiddleware(httpsserver::HTTPRequest* req,
                                     httpsserver::HTTPResponse* res,
                                     std::function<void()> next) {
    // Setup has no persistent streams. Release each slot after its response.
    res->setHeader("Connection", "close");
    res->setHeader("Cache-Control", "no-store");
    next();
}

// Handler functions for the HTTP setup server
void handleRoot(httpsserver::HTTPRequest * req, httpsserver::HTTPResponse * res) {
    res->setHeader("Content-Type", "text/html");
    if (req->getMethod() == "GET") res->print(configManager.portalHTML());
}

void handleScan(httpsserver::HTTPRequest * req, httpsserver::HTTPResponse * res) {
    res->setHeader("Content-Type", "application/json");
    res->print(configManager.scanNetworksJSON());
}

// Decode an application/x-www-form-urlencoded value: '+' -> space, '%xx' -> byte.
// Anything not matching is passed through unchanged, so a stray '%' is preserved
// rather than swallowed.
static String urlDecode(const String& src) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    String out;
    out.reserve(src.length());
    int len = src.length();
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < len) {
            int h1 = hexVal(src[i + 1]);
            int h2 = hexVal(src[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                out += (char)((h1 << 4) | h2);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

void handleSave(httpsserver::HTTPRequest * req, httpsserver::HTTPResponse * res) {
    res->setHeader("Content-Type", "application/json");

    if (req->getMethod() == "POST") {
        // Read POST body. The client posts application/x-www-form-urlencoded
        // (see portalHTML — URLSearchParams body); we parse it manually since
        // esp32_idf5_https_server doesn't have a built-in form parser.
        //
        // readBytes() is non-blocking and returns 0 whenever the TCP
        // receive buffer is momentarily empty — NOT only at end-of-body — so
        // we must drive the loop off requestComplete(). The body
        // can span multiple TCP packets on a real client (phone over the
        // AP), and the old `while (readBytes() > 0)` loop would bail out
        // after the first chunk and miss `ssid=`.
        std::string body;
        uint8_t buf[128];
        uint32_t deadline = millis() + 5000;
        while (!req->requestComplete() && millis() < deadline) {
            size_t len = req->readBytes(buf, sizeof(buf));
            if (len > 0) {
                body.append((char*)buf, len);
                if (body.size() > 4096) break;  // sanity cap
            } else {
                delay(1);
            }
        }
        Serial.printf("portal /save: read %u body bytes\n", (unsigned)body.size());

        String bodyStr = String(body.c_str());
        String ssid = "", wifiPass = "", webUser = "", webPass = "";

        int pos = 0;
        int bodyLen = bodyStr.length();
        while (pos < bodyLen) {
            int ampPos = bodyStr.indexOf('&', pos);
            if (ampPos < 0) ampPos = bodyLen;
            String pair = bodyStr.substring(pos, ampPos);
            int eqPos = pair.indexOf('=');
            if (eqPos >= 0) {
                String key   = urlDecode(pair.substring(0, eqPos));
                String value = urlDecode(pair.substring(eqPos + 1));

                if      (key == "ssid")     ssid     = value;
                else if (key == "wifiPass") wifiPass = value;
                else if (key == "webUser")  webUser  = value;
                else if (key == "webPass")  webPass  = value;
            }
            pos = ampPos + 1;
        }
        
        if (ssid.length() == 0) {
            res->setStatusCode(400);
            res->print("{\"error\":\"no ssid\"}");
        } else {
            // Save configuration — including WiFi creds so they survive a
            // reboot (arduino-esp32 3.x no longer auto-persists WiFi.begin()
            // credentials).
            const DeviceConfig& cfg = configManager.get();
            DeviceConfig newCfg = cfg;
            newCfg.configured = true;
            strncpy(newCfg.wifiSSID,     ssid.c_str(),     sizeof(newCfg.wifiSSID)     - 1);
            newCfg.wifiSSID[sizeof(newCfg.wifiSSID) - 1] = '\0';
            strncpy(newCfg.wifiPassword, wifiPass.c_str(), sizeof(newCfg.wifiPassword) - 1);
            newCfg.wifiPassword[sizeof(newCfg.wifiPassword) - 1] = '\0';
            if (webUser.length() > 0) {
                strncpy(newCfg.webUsername, webUser.c_str(), sizeof(newCfg.webUsername) - 1);
                newCfg.webUsername[sizeof(newCfg.webUsername) - 1] = '\0';
            }
            if (webPass.length() > 0) {
                strncpy(newCfg.webPassword, webPass.c_str(), sizeof(newCfg.webPassword) - 1);
                newCfg.webPassword[sizeof(newCfg.webPassword) - 1] = '\0';
            }
            if (!configManager.update(newCfg)) {
                res->setStatusCode(500);
                res->print("{\"error\":\"configuration save failed\"}");
                return;
            }

            res->print("{\"status\":\"ok\"}");

            // Connect to WiFi
            delay(500);
            WiFi.mode(WIFI_AP_STA);
            WiFi.begin(ssid.c_str(), wifiPass.c_str());

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
                configManager.serviceIdle();
                delay(5);
            }

            if (WiFi.status() != WL_CONNECTED) {
                WiFi.mode(WIFI_AP);
            }
        }
    } else {
        res->setStatusCode(405);
        res->print("{\"error\":\"method not allowed\"}");
    }
}

void ConfigManager::startPortalServer() {
#if TLS_ENABLED
    if (!portalCert_init() || !(sslCert = portalCert_newSSLCert())) {
        Serial.println("ERROR: Failed to initialize portal certificate");
        return;
    }
    // TLS record buffers are large on Arduino 2.x. Setup needs no streams,
    // so serialize requests and release the session after every response.
    portalServer = new httpsserver::HTTPSServer(sslCert, WEB_PORT, 1, WiFi.softAPIP());
#else
    portalServer = new httpsserver::HTTPServer(HTTP_PORT, 4, WiFi.softAPIP());
#endif
    portalServer->addMiddleware(&portalResponseMiddleware);
    portalServer->registerNode(new httpsserver::ResourceNode("/",     "GET",  &handleRoot));
    portalServer->registerNode(new httpsserver::ResourceNode("/",     "HEAD", &handleRoot));
    portalServer->registerNode(new httpsserver::ResourceNode("/scan", "GET",  &handleScan));
    portalServer->registerNode(new httpsserver::ResourceNode("/save", "POST", &handleSave));
    portalServer->setDefaultNode(&portalRedirectNode);
    if (!portalServer->start()) {
        Serial.printf("ERROR: Failed to start portal server on :%u\n", WEB_PORT);
        stopPortalServer();
        return;
    }
    Serial.printf("Portal server started: %s%s/\n",
                  WEB_SCHEME, WiFi.softAPIP().toString().c_str());
#if TLS_ENABLED
    portalRedirectServer = new httpsserver::HTTPServer(HTTP_PORT, 2, WiFi.softAPIP());
    portalRedirectServer->setDefaultNode(&portalRedirectNode);
    if (!portalRedirectServer->start()) {
        Serial.println("ERROR: Failed to start portal HTTP redirect; use HTTPS directly");
        delete portalRedirectServer;
        portalRedirectServer = nullptr;
    }
#endif
}

void ConfigManager::stopPortalServer() {
#if TLS_ENABLED
    if (portalRedirectServer) {
        portalRedirectServer->stop();
        delete portalRedirectServer;
        portalRedirectServer = nullptr;
    }
#endif
    if (portalServer) {
        portalServer->stop();
        delete portalServer;
        portalServer = nullptr;
    }
#if TLS_ENABLED
    delete sslCert;
    sslCert = nullptr;
#endif
}

ConfigManager::ConfigManager() {
    memset(&m_config, 0, sizeof(DeviceConfig));
}

bool ConfigManager::begin() {
    if (load()) return true;
    loadDefaults();
    return save(get());
}

DeviceConfig ConfigManager::get() const {
    portENTER_CRITICAL(&m_configMux);
    const DeviceConfig config = m_config;
    portEXIT_CRITICAL(&m_configMux);
    return config;
}

bool ConfigManager::update(const DeviceConfig& config) {
    DeviceConfig candidate = config;
    candidate.configured = true;
    if (!ldg::validDeviceConfig(candidate) || !save(candidate)) return false;
    portENTER_CRITICAL(&m_configMux);
    m_config = candidate;
    portEXIT_CRITICAL(&m_configMux);
    return true;
}

void ConfigManager::serviceIdle() {
    if (m_portalIdleCb) m_portalIdleCb();
    else delay(1);
}

String ConfigManager::portalHTML() {
    return R"RAW(<!DOCTYPE html>
<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LDG Tuner Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0a0a1a;color:#e8e8f0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#1a1d2e;border:1px solid #2a2d3e;border-radius:10px;padding:24px;width:100%;max-width:420px}
h2{color:#22c55e;font-size:18px;margin-bottom:4px}
p.sub{color:#6b7280;font-size:12px;margin-bottom:16px}
label{display:block;font-size:12px;color:#888;margin-bottom:4px;margin-top:12px}
input{width:100%;padding:10px;background:#151725;border:1px solid #2a2d3e;border-radius:6px;color:#e8e8f0;font-size:14px;outline:none}
input:focus{border-color:#22c55e}
button{width:100%;padding:12px;margin-top:20px;background:#22c55e;color:#000;border:none;border-radius:6px;font-size:14px;font-weight:600;cursor:pointer}
button:hover{background:#16a34a}
button.secondary{background:transparent;color:#22c55e;border:1px solid #22c55e;margin-top:10px}
.step{display:none}.step.active{display:block}
.net-item{padding:8px 10px;border-bottom:1px solid #2a2d3e;cursor:pointer;font-size:13px}
.net-item:hover{background:#1a1d2e}
.net-item.selected{background:#22c55e22}
.net-rssi{float:right;color:#6b7280;font-size:11px}
.lock{color:#666;font-size:11px}
#netList{max-height:200px;overflow-y:auto;border:1px solid #2a2d3e;border-radius:6px;margin-top:4px}
</style></head><body>
<div class='card'>
<h2>LDG Tuner Setup</h2>
<p class='sub'>Configure your device</p>
<div class='step active' id='step1'>
<h3 style='font-size:14px;margin-bottom:8px'>Select WiFi Network</h3>
<div id='netList'><div style='padding:12px;color:#6b7280;text-align:center'>Scanning...</div></div>
<label>WiFi Password</label>
<input type='password' id='wifiPass' placeholder='Network password'>
<button onclick='doStep1()'>Next</button>
</div>
<div class='step' id='step2'>
<h3 style='font-size:14px;margin-bottom:8px'>Web UI Password</h3>
<label>Web Username</label>
<input type='text' id='webUser' value='admin'>
<label>Web Password</label>
<input type='password' id='webPass' placeholder='Choose a strong password'>
<button onclick='doStep2()'>Save &amp; Connect</button>
<button class='secondary' onclick='goStep(1)'>Back</button>
</div>
<div class='step' id='step3'>
<h3 style='font-size:14px;margin-bottom:8px'>Connecting...</h3>
<p class='sub'>The device is connecting to your network.</p>
</div>
</div>
<script>
let selectedSSID='';
function scan(){fetch('/scan').then(r=>r.json()).then(nets=>{
var nl=document.getElementById('netList');nl.textContent='';
if(!nets.length){nl.textContent='No networks found';return;}
nets.forEach(n=>{
var item=document.createElement('div');
item.className='net-item';
item.addEventListener('click',function(){selNet(item,n.ssid);});
item.textContent=n.ssid;
var rssi=document.createElement('span');
rssi.className='net-rssi';
rssi.textContent=n.rssi+'dBm';
item.appendChild(rssi);
if(n.secure){var lk=document.createElement('span');lk.className='lock';lk.textContent='\uD83D\uDD12';item.appendChild(lk);}
nl.appendChild(item);
});
}).catch(()=>{var nl=document.getElementById('netList');nl.textContent='';var d=document.createElement('div');d.style.cssText='padding:12px;color:#ef4444;text-align:center';d.textContent='Scan failed';nl.appendChild(d);});
}
function selNet(el,ssid){document.querySelectorAll('.net-item').forEach(e=>e.classList.remove('selected'));el.classList.add('selected');selectedSSID=ssid;}
function goStep(n){document.querySelectorAll('.step').forEach(s=>s.classList.remove('active'));document.getElementById('step'+n).classList.add('active');}
function doStep1(){if(!selectedSSID){alert('Select a network');return;}goStep(2);}
function doStep2(){
let d=new URLSearchParams();
d.append('ssid',selectedSSID);d.append('wifiPass',document.getElementById('wifiPass').value);
d.append('webUser',document.getElementById('webUser').value);
d.append('webPass',document.getElementById('webPass').value);
goStep(3);
fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d.toString()}).then(r=>r.json()).then(d=>{
if(d.status==='ok'){setTimeout(()=>location.reload(),12000);}
else{alert('Error: '+d.error);goStep(2);}
}).catch(()=>{setTimeout(()=>location.reload(),12000);});
}
scan();
</script></body></html>)RAW";
}

String ConfigManager::scanNetworksJSON() {
    WiFi.scanNetworks(true);
    int n;
    while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) { serviceIdle(); delay(5); }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String r;
    serializeJson(doc, r);
    return r;
}

bool ConfigManager::setupWiFi() {
    // Try saved credentials from NVS. WiFi.SSID() can't be used here — under
    // arduino-esp32 3.x it returns the *currently connected* AP, which is
    // always empty at boot. We own STA creds via DeviceConfig.
    if (m_config.configured && m_config.wifiSSID[0] != '\0') {
        Serial.printf("Attempting to connect to saved network: %s\n", m_config.wifiSSID);
        WiFi.mode(WIFI_STA);
        if (m_config.useStaticIP && m_config.staticIP[0] != '\0') {
            IPAddress ip, mask, gw, dns;
            ip.fromString(m_config.staticIP);
            mask.fromString(m_config.staticNetmask);
            gw.fromString(m_config.staticGateway);
            dns.fromString(m_config.staticDNS);
            WiFi.config(ip, gw, mask, dns);
            Serial.printf("Static IP configured: %s\n", m_config.staticIP);
        }
        WiFi.begin(m_config.wifiSSID, m_config.wifiPassword);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            serviceIdle();
            delay(5);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        Serial.println("\nFailed to connect to saved network");
    }

    // Start configuration portal
    Serial.println("Starting configuration portal...");
    WiFi.softAP("LDGConfig", "configure");
    delay(100);
    Serial.printf("AP started: %s\n", WiFi.softAPIP().toString().c_str());

    dnsServer = new DNSServer();
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(53, "*", WiFi.softAPIP());

    // Start captive portal web server
    startPortalServer();

    // Portal runs until the user successfully joins a network. There is no
    // other configuration path on this device, so a hard timeout would brick
    // first-boot setup for any user who doesn't finish quickly enough.
    uint32_t nextHeapLog = millis();
    while (true) {
        dnsServer->processNextRequest();
#if TLS_ENABLED
        if (portalRedirectServer) portalRedirectServer->loop();
#endif
        if (portalServer) portalServer->loop();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nConnected: %s — rebooting to release :%u cleanly\n",
                          WiFi.localIP().toString().c_str(), WEB_PORT);
            // esp32_idf5_https_server's HTTPServer::setupSocket() binds without
            // SO_REUSEADDR. If we just tore the portal HTTPServer down here and
            // then tried to bind the runtime server on the same port, lwIP
            // would return EADDRINUSE because the previous listening socket
            // sits in TIME_WAIT. Reboot instead — on next boot, setupWiFi()'s
            // fast-path joins STA without ever starting the portal server, and
            // WEB_PORT is free for the runtime to bind.
            //
            // Brief drain so the /save response actually reaches the
            // client before the reset; the portal JS already plans for the
            // device to disappear (it does a delayed location.reload).
            delay(3000);
            ESP.restart();
            return true;  // unreachable, keeps the compiler happy
        }

        if ((int32_t)(millis() - nextHeapLog) >= 0) {
            nextHeapLog += PORTAL_HEAP_LOG_INTERVAL_MS;
            const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            Serial.printf("portal: uptime=%lus heap_free=%u min_free=%u "
                          "internal_free=%u internal_max=%u ap_clients=%u\n",
                          (unsigned long)(millis() / 1000),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMinFreeHeap(),
                          (unsigned)heap_caps_get_free_size(caps),
                          (unsigned)heap_caps_get_largest_free_block(caps),
                          (unsigned)WiFi.softAPgetStationNum());
        }

        serviceIdle();
    }
}

bool ConfigManager::reset() {
    if (!m_prefs.begin("ldg-config", false)) return false;
    const bool cleared = m_prefs.clear();
    m_prefs.end();
    // The HTTP handler restarts immediately after a successful clear. Keep
    // the running snapshot intact until then, including its authentication.
    return cleared;
}

bool ConfigManager::isConfigured() const {
    return get().configured;
}

void ConfigManager::loadDefaults() {
    m_config.wifiSSID[0] = '\0';
    m_config.wifiPassword[0] = '\0';
    // MQTT is optional. Leave broker/creds empty by default — main.cpp uses
    // an empty broker as the "MQTT disabled" signal so a never-configured
    // device doesn't chatter at a fake default IP.
    m_config.mqttBroker[0]   = '\0';
    m_config.mqttUsername[0] = '\0';
    m_config.mqttPassword[0] = '\0';
    strncpy(m_config.webUsername, WEB_AUTH_USERNAME, sizeof(m_config.webUsername) - 1);
    strncpy(m_config.webPassword, WEB_AUTH_PASSWORD, sizeof(m_config.webPassword) - 1);
    m_config.meterPsuVoltage = METER_PSU_VOLTAGE;
    strncpy(m_config.ant1Name, "ANT 1", sizeof(m_config.ant1Name) - 1);
    strncpy(m_config.ant2Name, "ANT 2", sizeof(m_config.ant2Name) - 1);
    m_config.mqttPort = MQTT_PORT;
    m_config.mqttDiscoveryEnabled = true;
    m_config.remoteUnitId = 0;
    m_config.configured = false;
    m_config.useStaticIP = false;
    m_config.staticIP[0]      = '\0';
    m_config.staticNetmask[0] = '\0';
    m_config.staticGateway[0] = '\0';
    m_config.staticDNS[0]     = '\0';
    m_config.remoteHost[0] = '\0';
    m_config.remoteHost[sizeof(m_config.remoteHost) - 1] = '\0';
}

bool ConfigManager::save(const DeviceConfig& config) {
    if (!m_prefs.begin("ldg-config", false)) return false;
    const bool saved = ldg::writeConfig(m_prefs, config);
    m_prefs.end();
    return saved;
}

bool ConfigManager::load() {
    if (!m_prefs.begin("ldg-config", true)) return false;
    if (m_prefs.isKey("configV1")) {
        const bool loaded = ldg::readConfig(m_prefs, m_config);
        m_prefs.end();
        return loaded;
    }

    if (!m_prefs.isKey("configured")) {
        m_prefs.end();
        return false;
    }

    // MQTT broker may legitimately be empty — that just means MQTT is off.
    // Use `configured` (set after captive-portal save) as the "did setup run"
    // signal; don't conflate it with whether MQTT is wanted.
    strncpy(m_config.wifiSSID,     m_prefs.getString("wifiSSID", "").c_str(),     sizeof(m_config.wifiSSID)     - 1);
    strncpy(m_config.wifiPassword, m_prefs.getString("wifiPassword", "").c_str(), sizeof(m_config.wifiPassword) - 1);
    strncpy(m_config.mqttBroker,   m_prefs.getString("mqttBroker", "").c_str(),   sizeof(m_config.mqttBroker)   - 1);
    strncpy(m_config.mqttUsername, m_prefs.getString("mqttUsername", "").c_str(), sizeof(m_config.mqttUsername) - 1);
    strncpy(m_config.mqttPassword, m_prefs.getString("mqttPassword", "").c_str(), sizeof(m_config.mqttPassword) - 1);
    strncpy(m_config.webUsername, m_prefs.getString("webUsername", "").c_str(), sizeof(m_config.webUsername) - 1);
    strncpy(m_config.webPassword, m_prefs.getString("webPassword", "").c_str(), sizeof(m_config.webPassword) - 1);
    m_config.meterPsuVoltage = m_prefs.getFloat("meterPsuVoltage", METER_PSU_VOLTAGE);
    strncpy(m_config.ant1Name, m_prefs.getString("ant1Name", "ANT 1").c_str(), sizeof(m_config.ant1Name) - 1);
    strncpy(m_config.ant2Name, m_prefs.getString("ant2Name", "ANT 2").c_str(), sizeof(m_config.ant2Name) - 1);
    m_config.mqttPort = m_prefs.getUShort("mqttPort", MQTT_PORT);
    m_config.mqttDiscoveryEnabled = m_prefs.getBool("haDiscovery", true);
    m_config.remoteUnitId = m_prefs.getUChar("remoteUnitId", 0);
    m_config.configured = m_prefs.getBool("configured", false);
    m_config.useStaticIP = m_prefs.getBool("useStaticIP", false);
    strncpy(m_config.staticIP,      m_prefs.getString("staticIP",      "").c_str(), sizeof(m_config.staticIP)      - 1);
    strncpy(m_config.staticNetmask, m_prefs.getString("staticNetmask", "").c_str(), sizeof(m_config.staticNetmask) - 1);
    strncpy(m_config.staticGateway, m_prefs.getString("staticGateway", "").c_str(), sizeof(m_config.staticGateway) - 1);
    strncpy(m_config.staticDNS,     m_prefs.getString("staticDNS",     "").c_str(), sizeof(m_config.staticDNS)     - 1);
    strncpy(m_config.remoteHost,    m_prefs.getString("remoteHost", "").c_str(), sizeof(m_config.remoteHost) - 1);

    m_prefs.end();
    // Preserve legacy Wi-Fi credentials even if old firmware accepted an
    // invalid optional setting. Future saves use the validated atomic blob.
    if (!std::isfinite(m_config.meterPsuVoltage) || m_config.meterPsuVoltage < 8 || m_config.meterPsuVoltage > 18)
        m_config.meterPsuVoltage = METER_PSU_VOLTAGE;
    if (!m_config.mqttPort) m_config.mqttPort = MQTT_PORT;
    if (!m_config.webUsername[0]) strlcpy(m_config.webUsername, WEB_AUTH_USERNAME, sizeof(m_config.webUsername));
    if (!m_config.webPassword[0]) strlcpy(m_config.webPassword, WEB_AUTH_PASSWORD, sizeof(m_config.webPassword));
    DeviceConfig checked = m_config;
    checked.useStaticIP = false;
    if (!ldg::validDeviceConfig(m_config) && ldg::validDeviceConfig(checked)) m_config.useStaticIP = false;
    return ldg::validDeviceConfig(m_config);
}
