#ifndef TUNER_PROTOCOL_H
#define TUNER_PROTOCOL_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

#include "tuner_types.h"
#include "tuner_diagnostics.h"

// Callback type for meter updates
typedef void (*meter_callback_t)(const tuner_meter_t* meter);

class TunerProtocol {
public:
    TunerProtocol();
    ~TunerProtocol();

    // Initialize serial communication with tuner
    bool begin(uint8_t rxPin, uint8_t txPin, uint32_t baud);

    // Send commands to tuner
    bool toggleAntenna();
    bool selectAntenna(tuner_ant_t antenna);
    bool memoryTune();
    bool fullTune();
    bool bypass();
    bool setAutoMode();
    bool setManualMode();

    // Align the receive stream using the full documented synchronization string.
    bool sync();

    // Meter control
    bool startMeter();
    bool stopMeter();

    // Get current meter readings (updated by background task)
    tuner_state_t getState() const;
    ldg::TunerDiagnostics getDiagnostics() const;

    // Get tuner mode (derived from responses)
    tuner_mode_t getMode() const;
    tuner_ant_t getAntenna() const;

    // Background processing (call from loop)
    void process();

    // Set callback for meter updates
    void setMeterCallback(meter_callback_t callback);

    // Check if tuner is communicating
    bool isConnected() const;

    // Calculate power from raw meter value
    static double calculatePower(uint16_t raw, double psuVoltage = 13.8);

    // Get band name string
    static const char* bandToString(tuner_band_t band);

private:
    // Internal methods
    bool sendCommand(uint8_t cmd);
    bool sendWake();
    bool switchToControlMode();
    bool switchToMeterMode();
    char exchangeCommand(uint8_t cmd);
    void resetCommandTrace(uint8_t exchange);
    char controlResponse(uint8_t cmd);
    void publishSnapshot(bool received = false, bool meterReceived = false);
    char waitForResponse(uint8_t cmd, uint32_t timeoutMs);
    void processResponseByte(uint8_t b);
    void processMeterByte(uint8_t b);
    void publishMeterData();
    tuner_band_t decodeBand(uint16_t indicator);

    HardwareSerial* m_serial;
    tuner_meter_t m_meterData;
    bool m_hasMeterData = false;
    meter_callback_t m_meterCallback;

    // Meter parsing state
    uint8_t m_meterBuf[TUNER_METER_DATA_LEN];
    uint8_t m_meterIdx;
    uint8_t m_eomCount;

    // Mode tracking
    enum class State {
        METER,
        RESPONSE,
        ERROR
    };
    State m_state;

    tuner_mode_t m_mode;
    tuner_ant_t m_antenna;
    bool m_connected;
    bool m_meterActive;

    // Response handling
    volatile char m_pendingResponse;
    bool m_waitingForResponse;
    uint8_t m_expectedCommand = 0;
    uint8_t m_syncMatched = 0;
    uint32_t m_lastAckMs = 0;
    bool m_ackGuardActive = false;
    // A timeout leaves an ambiguous reply in flight. Require Z before another
    // control operation rather than attributing the late reply to that operation.
    bool m_needsSync = false;
    // UART-worker owned. Freeze the latest exchange separately from live RX.
    uint8_t m_exchange = 0;
    bool m_exchangeWritten = false, m_exchangeUartError = false;
    uint8_t m_commandRx[32]{};
    uint8_t m_commandRxNext = 0, m_commandRxCount = 0;
    uint32_t m_commandRxBytes = 0;
    uint32_t m_responseTimeoutMs = 0, m_responseWaitMs = 0;

    // Commands and process() belong to the UART worker. The recursive mutex
    // also protects the command/response sequence against accidental callers.
    SemaphoreHandle_t m_mutex;
    // Readers never wait for a control command or UART I/O.
    mutable portMUX_TYPE m_snapshotMux = portMUX_INITIALIZER_UNLOCKED;
    tuner_state_t m_snapshot;
    uint32_t m_lastMeterMs = 0;
    uint32_t m_lastResponseMs = 0;
    bool m_seenResponse = false;
    ldg::TunerDiagnostics m_diagnostics;
    uint8_t m_rxTailNext = 0;
    std::atomic<bool> m_rxErrorPending{false};
    static constexpr uint32_t TELEMETRY_TIMEOUT_MS = 3000;
};

#endif // TUNER_PROTOCOL_H
