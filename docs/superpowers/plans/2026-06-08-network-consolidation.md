# Network Consolidation Implementation Plan

**Goal:** Consolidate three builds into two, remove the softAP dependency between units, add static IP config to both builds, fix the web UI SWR meter and antenna buttons, and switch to deterministic mDNS hostnames.

**Architecture:** Both builds (`esp32-display`, `esp32-remote`) are standalone web servers with local tuner control. The display build optionally subscribes to a remote unit's SSE stream for LVGL updates and proxies commands to it over infrastructure WiFi. No softAP is needed after initial portal setup. `REMOTE_UNIT` define and all its ifdefs are removed.

**Tech Stack:** ESP32/Arduino, PlatformIO, esp32_idf5_https_server, LVGL, ArduinoJson, WiFiClientSecure, Preferences (NVS), LittleFS, HTML/JS Canvas

---

## File Map

| File | Change |
|---|---|
| `platformio.ini` | Remove `esp32-nodisplay` env; drop `-DREMOTE_UNIT` from `esp32-remote` |
| `include/config.h` | Remove `REMOTE_UNIT_ID` ifdef block; keep `HOSTNAME_PREFIX` (unused after Task 3) |
| `include/config_manager.h` | Add `useStaticIP`, `staticIP/Netmask/Gateway/DNS`, `remoteHost` to `DeviceConfig` |
| `src/config_manager.cpp` | `loadDefaults()`, `save()`, `load()`, `setupWiFi()` — static IP, AP teardown fix, mDNS hostname |
| `src/main.cpp` | Remove all `REMOTE_UNIT` ifdefs; add `WITH_DISPLAY` remote-client block; simplify `executeCommand()`, `loop()` |
| `src/web_server.cpp` | `h_config_get/post` add new fields; drop command SSE event; drop `pushCommandEvent()` |
| `include/web_server.h` | Drop `pushCommandEvent()` declaration; drop `SseState` command fields |
| `data/index.html` | Network settings section; antenna buttons; SWR meter scale fix |

---

### Task 1: Build system — remove nodisplay, drop REMOTE_UNIT flag

**Files:**
- Modify: `platformio.ini`

- [ ] **Step 1: Remove esp32-nodisplay env and drop REMOTE_UNIT from esp32-remote**

Replace the two envs at the bottom of `platformio.ini`:

```ini
[env:esp32-display]
extends = env
build_flags =
    ${common.build_flags}
    -DWITH_DISPLAY
    -DWITH_TOUCH

[env:esp32-remote]
extends = env
build_flags =
    ${common.build_flags}
```

(Delete the entire `[env:esp32-nodisplay]` block.)

- [ ] **Step 2: Verify esp32-remote still compiles (it will fail on REMOTE_UNIT ifdefs — that's expected)**

```bash
nix-shell -p platformio --run "pio run -e esp32-remote" 2>&1 | tail -5
```

Expected: build succeeds (REMOTE_UNIT ifdefs evaluate to the `#else` branches when the define is absent, which is valid C++). If it fails, note the error for Task 4.

- [ ] **Step 3: Commit**

```bash
git add platformio.ini
git commit -m "build: remove nodisplay env, drop REMOTE_UNIT flag from esp32-remote"
```

---

### Task 2: DeviceConfig — add static IP and remoteHost fields

**Files:**
- Modify: `include/config_manager.h`
- Modify: `src/config_manager.cpp`

- [ ] **Step 1: Add fields to DeviceConfig struct in `include/config_manager.h`**

After the `bool configured;` line add:

```cpp
    // Static IP (optional; DHCP used when useStaticIP is false)
    bool useStaticIP;
    char staticIP[16];
    char staticNetmask[16];
    char staticGateway[16];
    char staticDNS[16];
    // Display-build only: hostname/IP of remote unit to subscribe to
    char remoteHost[64];
```

- [ ] **Step 2: Update `loadDefaults()` in `src/config_manager.cpp`**

After the `m_config.configured = false;` line at the end of `loadDefaults()`:

```cpp
    m_config.useStaticIP = false;
    m_config.staticIP[0]      = '\0';
    m_config.staticNetmask[0] = '\0';
    m_config.staticGateway[0] = '\0';
    m_config.staticDNS[0]     = '\0';
    strncpy(m_config.remoteHost, "ldgcontrol-remote.local",
            sizeof(m_config.remoteHost) - 1);
    m_config.remoteHost[sizeof(m_config.remoteHost) - 1] = '\0';
```

- [ ] **Step 3: Update `save()` in `src/config_manager.cpp`**

After the `m_prefs.putBool("configured", ...)` line, before `m_prefs.end()`:

```cpp
    m_prefs.putBool("useStaticIP",    m_config.useStaticIP);
    m_prefs.putString("staticIP",      m_config.staticIP);
    m_prefs.putString("staticNetmask", m_config.staticNetmask);
    m_prefs.putString("staticGateway", m_config.staticGateway);
    m_prefs.putString("staticDNS",     m_config.staticDNS);
    m_prefs.putString("remoteHost",    m_config.remoteHost);
```

- [ ] **Step 4: Update `load()` in `src/config_manager.cpp`**

After the `m_config.configured = m_prefs.getBool(...)` line, before `m_prefs.end()`:

```cpp
    m_config.useStaticIP = m_prefs.getBool("useStaticIP", false);
    strncpy(m_config.staticIP,      m_prefs.getString("staticIP",      "").c_str(), sizeof(m_config.staticIP)      - 1);
    strncpy(m_config.staticNetmask, m_prefs.getString("staticNetmask", "").c_str(), sizeof(m_config.staticNetmask) - 1);
    strncpy(m_config.staticGateway, m_prefs.getString("staticGateway", "").c_str(), sizeof(m_config.staticGateway) - 1);
    strncpy(m_config.staticDNS,     m_prefs.getString("staticDNS",     "").c_str(), sizeof(m_config.staticDNS)     - 1);
    strncpy(m_config.remoteHost,    m_prefs.getString("remoteHost", "ldgcontrol-remote.local").c_str(), sizeof(m_config.remoteHost) - 1);
```

- [ ] **Step 5: Build both envs**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -e esp32-remote" 2>&1 | tail -5
```

Expected: `2 succeeded`

- [ ] **Step 6: Commit**

```bash
git add include/config_manager.h src/config_manager.cpp
git commit -m "config: add static IP and remoteHost fields to DeviceConfig"
```

---

### Task 3: setupWiFi — static IP application, AP teardown fix, mDNS hostname

**Files:**
- Modify: `src/config_manager.cpp` (setupWiFi)
- Modify: `src/main.cpp` (setupWiFi wrapper)

- [ ] **Step 1: Apply static IP before `WiFi.begin()` in `ConfigManager::setupWiFi()`**

In `src/config_manager.cpp`, replace the block that starts with:
```cpp
    if (m_config.configured && m_config.wifiSSID[0] != '\0') {
        Serial.printf("Attempting to connect to saved network: %s\n", m_config.wifiSSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(m_config.wifiSSID, m_config.wifiPassword);
```

With:
```cpp
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
```

- [ ] **Step 2: Remove the `#ifndef WITH_DISPLAY` guard so the AP always drops after STA join**

Find this block in `ConfigManager::setupWiFi()` (inside the `while(true)` portal loop, after `stopPortalServer()`):

```cpp
#ifndef WITH_DISPLAY
            // Drop the softAP — only display units need to keep the AP up to
            // serve the remote-unit link. Reconfiguration after this point
            // happens via a reset (POST /api/config {"reset":true} or NVS
            // wipe) which boots back into the portal flow.
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
#endif
```

Replace with:

```cpp
            // Drop the softAP — the AP was only needed for the portal.
            // Both builds operate purely on the infrastructure network.
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
```

- [ ] **Step 3: Change mDNS hostname in `src/main.cpp` `setupWiFi()`**

Replace:

```cpp
void setupWiFi() {
    String hostname = HOSTNAME_PREFIX;
    hostname += "-";
    hostname += WiFi.macAddress();
    hostname.replace(":", "");

    WiFi.setHostname(hostname.c_str());
```

With:

```cpp
void setupWiFi() {
#ifdef WITH_DISPLAY
    const char* hostname = "ldgcontrol-display";
#else
    const char* hostname = "ldgcontrol-remote";
#endif

    WiFi.setHostname(hostname);
```

Also update the mDNS service registration further down in that function — change:

```cpp
        if (MDNS.begin(hostname.c_str())) {
            Serial.printf("mDNS responder started: %s.local\n", hostname.c_str());
```

To:

```cpp
        if (MDNS.begin(hostname)) {
            Serial.printf("mDNS responder started: %s.local\n", hostname);
```

- [ ] **Step 4: Build both envs**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -e esp32-remote" 2>&1 | tail -5
```

Expected: `2 succeeded`

- [ ] **Step 5: Commit**

```bash
git add src/config_manager.cpp src/main.cpp
git commit -m "wifi: static IP support, always drop AP after portal, ldgcontrol-* mDNS hostnames"
```

---

### Task 4: main.cpp — remove all REMOTE_UNIT ifdefs

**Files:**
- Modify: `src/main.cpp`

This task removes the old remote-unit client code. The display-build replacement comes in Task 5.

- [ ] **Step 1: Remove the WiFiClientSecure include and REMOTE_UNIT SSE state variables**

Delete this entire block (lines ~40–58):

```cpp
#ifdef REMOTE_UNIT
// SSE client for remote unit.
// ...threat model comment...
#include <WiFiClientSecure.h>
static WiFiClientSecure s_sseClient;
static bool s_sseConnected = false;
static uint32_t s_lastSseReconnect = 0;
#endif
```

Add `#include <WiFiClientSecure.h>` unconditionally near the top of the file (after the other includes) since Task 5 will use it for the display build:

```cpp
#include <WiFiClientSecure.h>
```

- [ ] **Step 2: Delete connectSSEClient(), processSSEClient(), postTelemetry()**

Delete everything from `#ifdef REMOTE_UNIT` (line ~154) through `#endif // REMOTE_UNIT` (line ~307). That includes the three functions and the opening `#ifdef REMOTE_UNIT` guard.

- [ ] **Step 3: Simplify executeCommand()**

Replace the entire `executeCommand()` function body:

```cpp
bool executeCommand(uint8_t cmd) {
    if (remoteUnitConnected) {
        // Proxy to remote unit — implemented in Task 5 for WITH_DISPLAY builds.
        // Until then, fall through to local execution.
    }
    bool result = false;
    switch (cmd) {
        case TUNER_CMD_TOGGLE_ANT:  result = tuner.toggleAntenna(); break;
        case TUNER_CMD_MEM_TUNE:    result = tuner.memoryTune();    break;
        case TUNER_CMD_FULL_TUNE:   result = tuner.fullTune();      break;
        case TUNER_CMD_BYPASS:      result = tuner.bypass();        break;
        case TUNER_CMD_AUTO_MODE:   result = tuner.setAutoMode();   break;
        case TUNER_CMD_MANUAL_MODE: result = tuner.setManualMode(); break;
    }
    return result;
}
```

(The `remoteUnitConnected` proxy branch gets its real implementation in Task 5.)

- [ ] **Step 4: Fix setup() — remove REMOTE_UNIT ifdefs**

In `setup()`, replace:

```cpp
#ifdef REMOTE_UNIT
    tuner.setMeterCallback(nullptr);
#else
    tuner.setMeterCallback(onMeterUpdate);
#endif
```

With:

```cpp
    tuner.setMeterCallback(onMeterUpdate);
```

Replace:

```cpp
    if (wifiConnected) {
        static WiFiClient mqttClient;
        mqtt.begin(mqttClient, executeCommand);

#ifndef REMOTE_UNIT
        mqtt.subscribeRemoteTelemetry(onRemoteTelemetry);
        mqtt.subscribeRemoteStatus(onRemoteStatus);
#endif
        ...
    }

#ifndef REMOTE_UNIT
    if (!webServer.begin(...)) { ... }
#endif
```

With:

```cpp
    if (wifiConnected) {
        static WiFiClient mqttClient;
        mqtt.begin(mqttClient, executeCommand);
        mqtt.subscribeRemoteTelemetry(onRemoteTelemetry);
        mqtt.subscribeRemoteStatus(onRemoteStatus);
    }

    if (!webServer.begin(&tuner, executeCommand, getActiveMeter, onRemoteTelemetry)) {
        Serial.println("ERROR: web server failed to start - port 443 may still be bound");
    }
```

Also update the build string in `setup()`:

```cpp
    Serial.printf("Build: %s %s\n",
#ifdef WITH_DISPLAY
        "display"
#else
        "remote"
#endif
    , __DATE__);
```

- [ ] **Step 5: Fix loop() — remove REMOTE_UNIT ifdefs**

Replace the `#ifdef REMOTE_UNIT ... #else ... #endif` block in `loop()`:

```cpp
#ifdef REMOTE_UNIT
    // Remote unit: SSE client + telemetry POST
    connectSSEClient();
    processSSEClient();

    if (wifiConnected && millis() - lastMeterPublish > 200) {
        lastMeterPublish = millis();
        const tuner_meter_t* meter = tuner.getMeterData();
        if (meter->forward_power_raw > 0 || meter->reflected_power_raw > 0) {
            postTelemetry(meter);
        }
    }
#else
    // Display unit: MQTT + web server
    if (wifiConnected) {
        mqtt.loop();

        if (!usingRemoteTuner && millis() - lastMeterPublish > 2000) {
            lastMeterPublish = millis();
            const tuner_meter_t* meter = tuner.getMeterData();
            if (meter->forward_power_raw > 0 || meter->reflected_power_raw > 0) {
                mqtt.publishTelemetry(meter);
            }
        }

        if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = millis();
            mqtt.publishStatus(remoteUnitConnected ? "remote_connected" : "online");
        }
    }

    webServer.loop();
#endif
```

With:

```cpp
    if (wifiConnected) {
        mqtt.loop();

        if (!usingRemoteTuner && millis() - lastMeterPublish > 2000) {
            lastMeterPublish = millis();
            const tuner_meter_t* meter = tuner.getMeterData();
            if (meter->forward_power_raw > 0 || meter->reflected_power_raw > 0) {
                mqtt.publishTelemetry(meter);
            }
        }

        if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = millis();
            mqtt.publishStatus(remoteUnitConnected ? "remote_connected" : "online");
        }
    }

    webServer.loop();
```

Also remove the now-unused `lastMeterPublish` variable from the globals (it was used in the REMOTE_UNIT telemetry publish path; the display path uses its own local millis check which still references it — keep it).

- [ ] **Step 6: Build both envs**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -e esp32-remote" 2>&1 | tail -5
```

Expected: `2 succeeded`

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "main: remove all REMOTE_UNIT ifdefs, both builds run full web server"
```

---

### Task 5: Display-build remote client (SSE subscriber + command proxy)

**Files:**
- Modify: `src/main.cpp`

This adds the optional remote unit connection to the display build. When `remoteHost` is configured and reachable, the display unit subscribes to the remote's `/api/events` SSE stream and proxies commands.

- [ ] **Step 1: Add a base64 helper and remote-client state variables near the top of main.cpp, inside `#ifdef WITH_DISPLAY`**

After the `#ifdef WITH_DISPLAY` display instance declaration, add:

```cpp
#ifdef WITH_DISPLAY
static WiFiClientSecure s_remoteClient;
static bool             s_remoteConnected  = false;
static uint32_t         s_lastRemoteReconnect = 0;

// Returns base64(user:pass) for HTTP Basic Auth headers.
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
```

- [ ] **Step 2: Add `connectRemoteUnit()` and `processRemoteSSE()` functions inside `#ifdef WITH_DISPLAY`**

Add these before `executeCommand()`:

```cpp
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

    // Try numeric IP first; fall back to mDNS/DNS resolution.
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
                  remoteIP.toString().c_str(), HTTPS_PORT);
    s_remoteClient.setInsecure();

    if (!s_remoteClient.connect(remoteIP, HTTPS_PORT)) {
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
    if (!s_remoteConnected || !s_remoteClient.connected()) {
        if (s_remoteConnected) {
            Serial.println("Remote: SSE disconnected");
            s_remoteClient.stop();
            s_remoteConnected = false;
            onRemoteStatus(false);
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

        // Meter event
        if (doc["fwd_power"].is<float>() || doc["fwd_power"].is<int>()) {
            tuner_meter_t m = {};
            m.forward_power_watts   = doc["fwd_power"]  | 0.0;
            m.reflected_power_watts = doc["ref_power"]  | 0.0;
            m.swr                   = doc["swr"]        | 1.0;
            m.forward_power_raw     = doc["fwd_raw"]    | 0;
            m.reflected_power_raw   = doc["ref_raw"]    | 0;
            onRemoteTelemetry(&m);

            // Mode and antenna come in the same JSON payload.
            if (doc["mode"].is<int>())
                display.updateStatus((tuner_mode_t)doc["mode"].as<int>(),
                                     (tuner_ant_t)doc["antenna"].as<int>());

            if (!remoteUnitConnected) onRemoteStatus(true);
        }
    }
}

// Proxy a command byte to the remote unit via HTTPS POST.
// Returns true on HTTP 200, false on connection failure or non-200 response.
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

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(remoteIP, HTTPS_PORT)) return false;

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

    // Read status line (timeout 3s)
    uint32_t deadline = millis() + 3000;
    while (!client.available() && millis() < deadline) delay(10);
    String status = client.readStringUntil('\n');
    client.stop();
    return status.indexOf("200") >= 0;
}
#endif // WITH_DISPLAY
```

- [ ] **Step 3: Update `executeCommand()` to use the proxy when remote is connected (display build)**

Replace the `executeCommand()` body from Task 4 with:

```cpp
bool executeCommand(uint8_t cmd) {
#ifdef WITH_DISPLAY
    if (remoteUnitConnected) {
        return proxyCommandToRemote(cmd);
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
    return result;
}
```

- [ ] **Step 4: Call connectRemoteUnit() and processRemoteSSE() from loop() under #ifdef WITH_DISPLAY**

In `loop()`, after `webServer.loop();`, add:

```cpp
#ifdef WITH_DISPLAY
    connectRemoteUnit();
    processRemoteSSE();
#endif
```

- [ ] **Step 5: Build esp32-display**

```bash
nix-shell -p platformio --run "pio run -e esp32-display" 2>&1 | tail -5
```

Expected: `SUCCESS`

- [ ] **Step 6: Build esp32-remote**

```bash
nix-shell -p platformio --run "pio run -e esp32-remote" 2>&1 | tail -5
```

Expected: `SUCCESS`

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "display: add remote unit SSE client and command proxy"
```

---

### Task 6: Web server API — new config fields, remove command SSE event

**Files:**
- Modify: `src/web_server.cpp`
- Modify: `include/web_server.h`

- [ ] **Step 1: Remove `commandSeq`/`commandValue` from `SseState` in `src/web_server.cpp`**

Replace the `SseState` struct:

```cpp
struct SseState {
    portMUX_TYPE mux;
    uint32_t meterSeq;
    char meterJson[384];
    uint32_t statusSeq;
    char statusJson[96];
};
static SseState s_sse = {
    portMUX_INITIALIZER_UNLOCKED,
    0, "", 0, ""
};
```

- [ ] **Step 2: Remove the command event dispatch block from `h_events()`**

In the SSE send loop, delete this block entirely:

```cpp
        if (cSeq != lastCommandSeq) {
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"command\":%u}", (unsigned)cmdVal);
            written = res->print("event: command\ndata: ");
            res->print(buf);
            res->print("\n\n");
            lastCommandSeq = cSeq;
        }
```

Also delete the `uint32_t lastCommandSeq`, `uint8_t cmdVal`, and `cSeq` variables in that loop, and the snapshot of `lastCommandSeq` and `commandValue` before the loop.

- [ ] **Step 3: Remove `pushCommandEvent()` from `src/web_server.cpp`**

Delete the entire `LdgWebServer::pushCommandEvent()` method body at the bottom of the file.

- [ ] **Step 4: Remove `pushCommandEvent()` declaration from `include/web_server.h`**

Delete:
```cpp
    // SSE: push command event to remote unit clients
    void pushCommandEvent(uint8_t cmd);
```

- [ ] **Step 5: Update `h_config_get` to include new network fields**

Replace the `h_config_get` handler body with:

```cpp
static void h_config_get(hsv::HTTPRequest* req, hsv::HTTPResponse* res) {
    if (!authenticate(req, res)) return;

    const DeviceConfig& cfg = configManager.get();
    JsonDocument doc;
    doc["mqttBroker"]      = cfg.mqttBroker;
    doc["mqttPort"]        = cfg.mqttPort;
    doc["mqttUsername"]    = cfg.mqttUsername;
    doc["webUsername"]     = cfg.webUsername;
    doc["meterPsuVoltage"] = cfg.meterPsuVoltage;
    doc["ant1Name"]        = cfg.ant1Name;
    doc["ant2Name"]        = cfg.ant2Name;
    doc["configured"]      = cfg.configured;
    // Static IP settings
    doc["useStaticIP"]     = cfg.useStaticIP;
    doc["staticIP"]        = cfg.staticIP;
    doc["staticNetmask"]   = cfg.staticNetmask;
    doc["staticGateway"]   = cfg.staticGateway;
    doc["staticDNS"]       = cfg.staticDNS;
    // Current live network values (always from DHCP/static, used to pre-fill UI)
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
```

- [ ] **Step 6: Update `h_config_post` to accept new fields**

After the existing `if (doc["ant2Name"])` line, add:

```cpp
    if (doc["useStaticIP"].is<bool>())  cfg.useStaticIP = doc["useStaticIP"];
    if (doc["staticIP"])      strncpy(cfg.staticIP,      doc["staticIP"],      sizeof(cfg.staticIP)      - 1);
    if (doc["staticNetmask"]) strncpy(cfg.staticNetmask, doc["staticNetmask"], sizeof(cfg.staticNetmask) - 1);
    if (doc["staticGateway"]) strncpy(cfg.staticGateway, doc["staticGateway"], sizeof(cfg.staticGateway) - 1);
    if (doc["staticDNS"])     strncpy(cfg.staticDNS,     doc["staticDNS"],     sizeof(cfg.staticDNS)     - 1);
#ifdef WITH_DISPLAY
    if (doc["remoteHost"])    strncpy(cfg.remoteHost, doc["remoteHost"], sizeof(cfg.remoteHost) - 1);
#endif
```

Also delete the `remoteUnitId` lines in both `h_config_get` and `h_config_post` since that field is no longer meaningful.

- [ ] **Step 7: Build both envs**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -e esp32-remote" 2>&1 | tail -5
```

Expected: `2 succeeded`

- [ ] **Step 8: Flash esp32-display and verify /api/config returns new fields**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -t upload --upload-port /dev/ttyUSB0" 2>&1 | tail -5
```

Then from a browser or curl, `GET https://<device-ip>/api/config` and verify the response includes `useStaticIP`, `currentIP`, `currentGateway`, `build`, etc.

- [ ] **Step 9: Commit**

```bash
git add src/web_server.cpp include/web_server.h
git commit -m "api: add static IP and build fields to /api/config, remove command SSE event"
```

---

### Task 7: Web UI — network settings section with static IP

**Files:**
- Modify: `data/index.html`

- [ ] **Step 1: Replace the settings Network section HTML**

Find and replace the entire `<div class="settings-section">` block that currently contains SSID/IP read-only fields:

```html
<div class="settings-section">
    <h3>Network</h3>
    <div class="form-group">
        <label>Connected SSID</label>
        <input type="text" id="ssid" readonly style="opacity: 0.7; cursor: not-allowed;">
    </div>
    <div class="form-group">
        <label>IP Address</label>
        <input type="text" id="wifiIP" readonly style="opacity: 0.7; cursor: not-allowed;">
    </div>
    <p style="font-size: 0.7rem; color: var(--text-secondary); margin-top: 0.4rem;">
        To change WiFi network, press the reset button on the device to launch the captive portal.
    </p>
</div>
```

Replace with:

```html
<div class="settings-section">
    <h3>Network</h3>
    <div class="form-group">
        <label>Connected SSID</label>
        <input type="text" id="ssid" readonly style="opacity: 0.7; cursor: not-allowed;">
    </div>
    <div class="form-row" style="align-items:center;margin-bottom:0.5rem;">
        <label style="margin:0;font-weight:600;">IP Mode</label>
        <label style="display:flex;align-items:center;gap:0.4rem;cursor:pointer;">
            <input type="checkbox" id="useStaticIP" onchange="toggleStaticIP()"> Static IP
        </label>
    </div>
    <div id="staticIPFields" style="display:none;">
        <div class="form-row">
            <div class="form-group">
                <label>IP Address</label>
                <input type="text" id="staticIP" placeholder="192.168.1.100" pattern="\d+\.\d+\.\d+\.\d+">
            </div>
            <div class="form-group">
                <label>Netmask</label>
                <input type="text" id="staticNetmask" placeholder="255.255.255.0">
            </div>
        </div>
        <div class="form-row">
            <div class="form-group">
                <label>Gateway</label>
                <input type="text" id="staticGateway" placeholder="192.168.1.1">
            </div>
            <div class="form-group">
                <label>DNS</label>
                <input type="text" id="staticDNS" placeholder="192.168.1.1">
            </div>
        </div>
    </div>
    <div id="dhcpInfo" style="font-size:0.75rem;color:var(--text-secondary);margin-top:0.25rem;">
        Current: <span id="currentIP">--</span> / <span id="currentNetmask">--</span>
        &nbsp;GW: <span id="currentGateway">--</span>
    </div>
    <div id="remoteHostGroup" style="display:none;margin-top:0.75rem;">
        <div class="form-group">
            <label>Remote Unit Host</label>
            <input type="text" id="remoteHost" placeholder="ldgcontrol-remote.local">
        </div>
    </div>
    <p style="font-size: 0.7rem; color: var(--text-secondary); margin-top: 0.4rem;">
        To change WiFi network, press the reset button on the device to launch the captive portal.
    </p>
</div>
```

- [ ] **Step 2: Add `toggleStaticIP()` to the JavaScript**

After the `document.querySelectorAll('.nav-tab')` event listener setup, add:

```javascript
function toggleStaticIP() {
    const on = document.getElementById('useStaticIP').checked;
    document.getElementById('staticIPFields').style.display = on ? '' : 'none';
}
```

- [ ] **Step 3: Update `loadSettings()` to populate the new fields**

In `loadSettings()`, add after the existing `if (data.wifiSSID)` lines:

```javascript
                document.getElementById('useStaticIP').checked = !!data.useStaticIP;
                toggleStaticIP();
                if (data.staticIP)      document.getElementById('staticIP').value      = data.staticIP;
                if (data.staticNetmask) document.getElementById('staticNetmask').value = data.staticNetmask;
                if (data.staticGateway) document.getElementById('staticGateway').value = data.staticGateway;
                if (data.staticDNS)     document.getElementById('staticDNS').value     = data.staticDNS;
                if (data.currentIP)     document.getElementById('currentIP').textContent     = data.currentIP;
                if (data.currentNetmask)document.getElementById('currentNetmask').textContent= data.currentNetmask;
                if (data.currentGateway)document.getElementById('currentGateway').textContent= data.currentGateway;
                // Pre-fill static fields with DHCP values if not already set
                if (!data.useStaticIP) {
                    if (!data.staticIP)      document.getElementById('staticIP').value      = data.currentIP      || '';
                    if (!data.staticNetmask) document.getElementById('staticNetmask').value = data.currentNetmask || '';
                    if (!data.staticGateway) document.getElementById('staticGateway').value = data.currentGateway || '';
                    if (!data.staticDNS)     document.getElementById('staticDNS').value     = data.currentDNS     || '';
                }
                // Show Remote Unit Host only on display build
                if (data.build === 'display') {
                    document.getElementById('remoteHostGroup').style.display = '';
                    if (data.remoteHost) document.getElementById('remoteHost').value = data.remoteHost;
                }
```

Also remove the existing `if (data.wifiIP)` line that set the old read-only IP input (that element is gone).

- [ ] **Step 4: Update `saveSettings()` to include new fields**

Add to the `config` object in `saveSettings()`:

```javascript
                useStaticIP:    document.getElementById('useStaticIP').checked,
                staticIP:       document.getElementById('staticIP').value,
                staticNetmask:  document.getElementById('staticNetmask').value,
                staticGateway:  document.getElementById('staticGateway').value,
                staticDNS:      document.getElementById('staticDNS').value,
                remoteHost:     document.getElementById('remoteHost').value,
```

- [ ] **Step 5: Upload LittleFS and verify in browser**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -t uploadfs --upload-port /dev/ttyUSB0" 2>&1 | tail -5
```

Open the Settings tab. Verify:
- SSID shows correctly
- Current IP/netmask/gateway shows
- Static fields are pre-filled with DHCP values but disabled
- Checking "Static IP" enables the fields
- Remote Unit Host field appears

- [ ] **Step 6: Commit**

```bash
git add data/index.html
git commit -m "ui: add static IP configuration and remote host to settings"
```

---

### Task 8: Web UI — antenna buttons (named, active/inactive)

**Files:**
- Modify: `data/index.html`

- [ ] **Step 1: Replace the Toggle Ant button and Antenna status row in the control tab HTML**

Find and remove:
```html
<div class="status-row"><span>Antenna</span><span id="antValue">--</span></div>
```

And find and replace:
```html
<button class="btn btn-blue" onclick="sendCommand('toggle')">Toggle Ant</button>
```

With:
```html
<div class="form-row" style="gap:0.5rem;margin-bottom:0.35rem;">
    <button class="btn ant-btn" id="btnAnt1" onclick="selectAntenna(1)">
        <span class="ant-hw">ANT 1</span>
        <span class="ant-label" id="ant1Label">ANT 1</span>
    </button>
    <button class="btn ant-btn" id="btnAnt2" onclick="selectAntenna(2)">
        <span class="ant-hw">ANT 2</span>
        <span class="ant-label" id="ant2Label">ANT 2</span>
    </button>
</div>
```

- [ ] **Step 2: Add antenna button styles**

In the `<style>` section, add:

```css
        .ant-btn { flex: 1; display: flex; flex-direction: column;
                   align-items: center; padding: 0.5rem 0.25rem; }
        .ant-btn .ant-hw { font-size: 0.65rem; opacity: 0.7; letter-spacing: 0.05em; }
        .ant-btn .ant-label { font-size: 0.9rem; font-weight: 600; }
        .ant-btn.active { background: var(--accent-green); color: #000; opacity: 1; }
        .ant-btn.inactive { background: #1f2937; color: #6b7280; opacity: 0.6; }
```

- [ ] **Step 3: Add `selectAntenna()` and `updateAntennaButtons()` to JavaScript**

```javascript
        // Clicking an already-active antenna is a no-op.
        // Clicking the inactive one sends toggle to switch to it.
        let activeAntenna = 0; // 0=unknown, 1=ANT_A, 2=ANT_B

        function selectAntenna(ant) {
            if (ant === activeAntenna) return;
            sendCommand('toggle');
        }

        function updateAntennaButtons(antenna) {
            activeAntenna = antenna;
            const b1 = document.getElementById('btnAnt1');
            const b2 = document.getElementById('btnAnt2');
            b1.className = 'btn ant-btn ' + (antenna === 1 ? 'active' : (antenna === 2 ? 'inactive' : ''));
            b2.className = 'btn ant-btn ' + (antenna === 2 ? 'active' : (antenna === 1 ? 'inactive' : ''));
        }
```

- [ ] **Step 4: Update `updateMeter()` to call `updateAntennaButtons()` and populate labels**

In `updateMeter()`, replace:
```javascript
            if (data.antenna !== undefined) {
                const ants = { 0: '--', 1: 'A', 2: 'B' };
                document.getElementById('antValue').textContent = ants[data.antenna] || '--';
            }
```

With:
```javascript
            if (data.antenna !== undefined) {
                updateAntennaButtons(data.antenna);
            }
```

- [ ] **Step 5: Update `loadSettings()` to populate ant labels on the buttons**

After the existing `if (data.ant1Name)` line add:
```javascript
                if (data.ant1Name) document.getElementById('ant1Label').textContent = data.ant1Name;
                if (data.ant2Name) document.getElementById('ant2Label').textContent = data.ant2Name;
```

- [ ] **Step 6: Upload LittleFS and verify**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -t uploadfs --upload-port /dev/ttyUSB0" 2>&1 | tail -5
```

Open the Control tab. Verify:
- Two antenna buttons appear side by side
- Button labels show the configured names (ANT 1 / ANT 2 by default)
- When SSE sends antenna data, the active button highlights green, inactive dims
- Clicking the inactive button sends the toggle command

- [ ] **Step 7: Commit**

```bash
git add data/index.html
git commit -m "ui: replace toggle button with named active/inactive antenna buttons"
```

---

### Task 9: Web UI — SWR meter scale fix (two 90° colored arcs)

**Files:**
- Modify: `data/index.html`

The current meter draws one gray 180° background arc (`Math.PI` to `2*Math.PI`). The needles each only sweep 90°. This task replaces the single arc with two colored 90° arcs that match each needle's sweep path exactly.

- [ ] **Step 1: Replace the gray semicircle arc with two colored quarter-circle arcs**

Find and replace this block in `drawMeter()`:

```javascript
            // Scale arc — upper semicircle from LEFT (φ=π) through TOP
            // (φ=3π/2) to RIGHT (φ=2π).
            ctx.strokeStyle = '#555';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(cx, cy, radius, Math.PI, 2 * Math.PI);
            ctx.stroke();
```

With:

```javascript
            // Two scale arcs — each covers only the 90° that its needle sweeps.
            // Forward arc: left (φ=π, zero) → top (φ=3π/2, full scale)
            ctx.strokeStyle = 'rgba(34,197,94,0.35)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(cx, cy, radius, Math.PI, 1.5 * Math.PI);
            ctx.stroke();
            // Reflected arc: right (φ=2π, zero) → top (φ=3π/2, full scale), clockwise=false
            ctx.strokeStyle = 'rgba(239,68,68,0.35)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(cx, cy, radius, 1.5 * Math.PI, 2 * Math.PI);
            ctx.stroke();
```

- [ ] **Step 2: Upload LittleFS and verify visually**

```bash
nix-shell -p platformio --run "pio run -e esp32-display -t uploadfs --upload-port /dev/ttyUSB0" 2>&1 | tail -5
```

Open the Control tab. Verify:
- The green arc runs from the left side (9 o'clock) to the top center (12 o'clock) — 90° sweep
- The red arc runs from the right side (3 o'clock) to the top center (12 o'clock) — 90° sweep
- Both arcs meet at the top center
- Scale tick marks sit outside each arc, matching their respective sweeps
- At 50% forward power the forward needle points midway between 9 o'clock and 12 o'clock, aligned with the 50% tick on the green arc

- [ ] **Step 3: Commit**

```bash
git add data/index.html
git commit -m "ui: fix SWR meter scales to two 90-degree arcs matching needle geometry"
```

---

## Requirements and tasks

| Spec requirement | Task |
|---|---|
| Remove esp32-nodisplay, esp32-remote = old nodisplay | Task 1 |
| Drop softAP after STA join for all builds | Task 3 |
| mDNS: ldgcontrol-display.local / ldgcontrol-remote.local | Task 3 |
| Static IP in DeviceConfig | Task 2 |
| Static IP applied in setupWiFi | Task 3 |
| remoteHost in DeviceConfig | Task 2 |
| Remove all REMOTE_UNIT ifdefs | Task 4 |
| Display build SSE client (connects to remote) | Task 5 |
| Command proxy (display → remote HTTPS POST) | Task 5 |
| /api/config GET: static IP + current* + build + remoteHost | Task 6 |
| /api/config POST: accept new fields | Task 6 |
| Remove command SSE event + pushCommandEvent | Task 6 |
| Web UI: static IP settings section | Task 7 |
| Web UI: pre-fill from DHCP, remoteHost shown for display | Task 7 |
| Web UI: named active/inactive antenna buttons | Task 8 |
| Web UI: SWR meter two 90° arcs | Task 9 |
