#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include "tuner_protocol.h"
#include "config.h"
#include "mqtt_status.h"
#include "meter_scale.h"

namespace httpsserver { class HTTPServer; }
#if TLS_ENABLED
namespace httpsserver { class SSLCert; }
#endif

typedef bool (*command_handler_t)(uint8_t cmd);
typedef tuner_state_t (*state_getter_t)();
typedef void (*remote_telemetry_handler_t)(const tuner_state_t& state);
typedef ldg::MqttStatus (*mqtt_status_getter_t)();

class LdgWebServer {
public:
    LdgWebServer();
    ~LdgWebServer();

    bool begin(TunerProtocol* tuner, command_handler_t cmdHandler = nullptr,
               state_getter_t stateGetter = nullptr,
               remote_telemetry_handler_t remoteTelemetryHandler = nullptr,
               mqtt_status_getter_t mqttStatusGetter = nullptr,
               ldg::MeterScale* meterScale = nullptr);
    void loop();

    // Publish a value snapshot for WebSocket subscribers.
    void pushMeterEvent(const tuner_state_t& state);

    // Accessors for the static handler functions (which receive a raw
    // HTTPRequest/HTTPResponse and need to reach back into instance state).
    TunerProtocol* getTuner() { return m_tuner; }
    command_handler_t getCmdHandler() { return m_cmdHandler; }
    state_getter_t getStateGetter() { return m_stateGetter; }
    remote_telemetry_handler_t getRemoteTelemetryHandler() { return m_remoteTelemetryHandler; }
    mqtt_status_getter_t getMqttStatusGetter() { return m_mqttStatusGetter; }
    ldg::MeterScale* getMeterScale() { return m_meterScale; }
    void setMqttReconfigureHandler(void (*handler)()) { m_mqttReconfigureHandler = handler; }
    bool requestMqttReconfigure() {
        if (!m_mqttReconfigureHandler) return false;
        m_mqttReconfigureHandler(); return true;
    }

private:
    httpsserver::HTTPServer* m_server;
#if TLS_ENABLED
    httpsserver::SSLCert* m_sslCert;
#endif
    TunerProtocol* m_tuner;
    command_handler_t m_cmdHandler;
    state_getter_t m_stateGetter;
    remote_telemetry_handler_t m_remoteTelemetryHandler;
    mqtt_status_getter_t m_mqttStatusGetter;
    ldg::MeterScale* m_meterScale;
    void (*m_mqttReconfigureHandler)() = nullptr;
    uint32_t m_lastMeterScaleRevision = 0;
    uint32_t m_lastMeterUpdate;
    uint32_t m_lastWsMeterSeq = 0;  // last meterSeq broadcast to WS subscribers
};

#endif // WEB_SERVER_H
