#pragma once
#include "Arduino.h"
#include "IPAddress.h"
static constexpr int WL_CONNECTED = 3;
struct WiFiClient { void setTimeout(int) {} };
struct FakeIP { String toString() const { return "192.0.2.1"; } };
struct FakeWiFi {
    int wifiStatus = WL_CONNECTED;
    String mac = "AA:BB:CC:01:02:03";
    int status() const { return wifiStatus; }
    String macAddress() const { return mac; }
    FakeIP localIP() const { return {}; }
};
extern FakeWiFi WiFi;
