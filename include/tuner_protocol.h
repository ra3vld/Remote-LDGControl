#ifndef TUNER_PROTOCOL_H
#define TUNER_PROTOCOL_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "tuner_types.h"

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
    bool memoryTune();
    bool fullTune();
    bool bypass();
    bool setAutoMode();
    bool setManualMode();

    // Query tuner state
    bool sync();

    // Meter control
    bool startMeter();
    bool stopMeter();

    // Get current meter readings (updated by background task)
    const tuner_meter_t* getMeterData() const;
    tuner_state_t getState() const;

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
    void switchToControlMode();
    void switchToMeterMode();
    char waitForResponse(uint32_t timeoutMs = 500);
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

    // Mutex protecting UART access from main and webServer FreeRTOS tasks.
    // Held by every high-level command (toggleAntenna, memoryTune, ...) for
    // the duration of its switchToControlMode → sendCommand → waitForResponse
    // → switchToMeterMode sequence so meter-mode bytes can't interleave with
    // control-mode responses. process() uses a non-blocking acquire: if the
    // mutex is busy, this tick's meter parse is skipped and bytes stay in the
    // UART RX FIFO until the next call — UART2's 256-byte buffer is plenty
    // for a worst-case command (~1.5s).
    SemaphoreHandle_t m_mutex;
};

#endif // TUNER_PROTOCOL_H
