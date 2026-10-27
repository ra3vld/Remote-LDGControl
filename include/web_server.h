#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include "tuner_protocol.h"
#include "config.h"

namespace httpsserver { class HTTPSServer; class SSLCert; }

typedef bool (*command_handler_t)(uint8_t cmd);
typedef tuner_state_t (*state_getter_t)();
typedef void (*remote_telemetry_handler_t)(const tuner_state_t& state);

class LdgWebServer {
public:
    LdgWebServer();
    ~LdgWebServer();

    bool begin(TunerProtocol* tuner, command_handler_t cmdHandler = nullptr,
               state_getter_t stateGetter = nullptr,
               remote_telemetry_handler_t remoteTelemetryHandler = nullptr);
    void loop();

    // SSE: push meter event to all connected clients
    void pushMeterEvent(const tuner_state_t& state);

    // SSE: push status event
    void pushStatusEvent(const char* status);

    // Accessors for the static handler functions (which receive a raw
    // HTTPRequest/HTTPResponse and need to reach back into instance state).
    TunerProtocol* getTuner() { return m_tuner; }
    command_handler_t getCmdHandler() { return m_cmdHandler; }
    state_getter_t getStateGetter() { return m_stateGetter; }
    remote_telemetry_handler_t getRemoteTelemetryHandler() { return m_remoteTelemetryHandler; }

private:
    httpsserver::HTTPSServer* m_server;
    httpsserver::SSLCert* m_sslCert;
    TunerProtocol* m_tuner;
    command_handler_t m_cmdHandler;
    state_getter_t m_stateGetter;
    remote_telemetry_handler_t m_remoteTelemetryHandler;
    uint32_t m_lastMeterUpdate;
    uint32_t m_lastWsMeterSeq = 0;  // last meterSeq broadcast to WS subscribers
};

#endif // WEB_SERVER_H
