#include "tuner_protocol.h"
#include "voltage_sensor.h"
#include "config.h"
#include <driver/uart.h>
#include <new>

namespace {
// RAII scoped acquire of the TunerProtocol mutex. Blocks indefinitely by
// default (control-command path); pass timeout=0 for the non-blocking try
// used by process(). The mutex is recursive because waitForResponse()
// internally calls process() from within an already-locked high-level
// method — without recursion the inner take would fail and we'd never
// read the response bytes off the UART.
struct TunerLock {
    SemaphoreHandle_t m;
    bool held;
    TunerLock(SemaphoreHandle_t mux, TickType_t timeout = portMAX_DELAY) : m(mux) {
        held = (m && xSemaphoreTakeRecursive(m, timeout) == pdTRUE);
    }
    ~TunerLock() { if (held && m) xSemaphoreGiveRecursive(m); }
    explicit operator bool() const { return held; }
};
}

TunerProtocol::TunerProtocol()
    : m_serial(nullptr),
      m_meterCallback(nullptr),
      m_meterIdx(0),
      m_eomCount(0),
      m_state(State::RESPONSE),
      m_mode(MODE_UNKNOWN),
      m_antenna(ANT_UNKNOWN),
      m_connected(false),
      m_meterActive(false),
      m_pendingResponse(0),
      m_waitingForResponse(false),
      m_mutex(nullptr)
{
    memset(&m_meterData, 0, sizeof(tuner_meter_t));
    memset(m_meterBuf, 0, sizeof(m_meterBuf));
}

TunerProtocol::~TunerProtocol() {
    if (m_serial) delete m_serial;
    if (m_mutex)  vSemaphoreDelete(m_mutex);
}

bool TunerProtocol::begin(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    m_mutex = xSemaphoreCreateRecursiveMutex();
    m_serial = new (std::nothrow) HardwareSerial(TUNER_UART_NUM);
    if (!m_mutex || !m_serial) return false;
    m_serial->begin(baud, SERIAL_8N1, rxPin, txPin);
    m_connected = true;

    // Start in meter mode to receive telemetry
    delay(100);
    return startMeter();
}

bool TunerProtocol::sendWake() {
    if (!m_serial || !m_connected) return false;

    for (int i = 0; i < 2; i++) {
        if (m_serial->write(TUNER_CMD_WAKE) != 1) {
            return false;
        }
        delay(TUNER_WAKE_DELAY_MS);
    }
    return true;
}

bool TunerProtocol::sendCommand(uint8_t cmd) {
    if (!m_serial || !m_connected) return false;

    if (!sendWake()) return false;

    if (m_serial->write(cmd) != 1) {
        return false;
    }

    // Per documentation: minimum 200ms before next command
    delay(TUNER_CMD_DELAY_MS);

    return true;
}

bool TunerProtocol::switchToControlMode() {
    m_state = State::RESPONSE;
    m_meterIdx = m_eomCount = 0;
    if (!sendCommand(TUNER_CMD_METER_STOP)) return false;
    // Discard the final meter frame that was already in flight before X.
    while (m_serial->available()) m_serial->read();
    return true;
}

bool TunerProtocol::switchToMeterMode() {
    m_meterIdx = m_eomCount = 0;
    m_state = State::METER;
    m_meterActive = sendCommand(TUNER_CMD_METER_START);
    return m_meterActive;
}

char TunerProtocol::waitForResponse(uint32_t timeoutMs) {
    const uint32_t start = millis();
    m_waitingForResponse = true;
    m_pendingResponse = 0;
    while (millis() - start < timeoutMs) {
        process();
        if (m_pendingResponse != 0) break;
        delay(1);
    }
    m_waitingForResponse = false;
    return m_pendingResponse ? m_pendingResponse : TUNER_RESP_ERROR;
}

char TunerProtocol::controlResponse(uint8_t cmd) {
    TunerLock lock(m_mutex);
    if (!lock) return TUNER_RESP_ERROR;
    char response = TUNER_RESP_ERROR;
    if (switchToControlMode() && sendCommand(cmd)) response = waitForResponse();
    if (cmd == TUNER_CMD_TOGGLE_ANT) {
        if (response == TUNER_RESP_ANT_A) m_antenna = ANT_A;
        else if (response == TUNER_RESP_ANT_B) m_antenna = ANT_B;
        else m_antenna = ANT_UNKNOWN;
    } else if (cmd == TUNER_CMD_AUTO_MODE && response == TUNER_RESP_AUTO) m_mode = MODE_AUTO;
    else if (cmd == TUNER_CMD_MANUAL_MODE && response == TUNER_RESP_MANUAL) m_mode = MODE_MANUAL;
    else if (cmd == TUNER_CMD_BYPASS && response == TUNER_RESP_BYPASS) m_mode = MODE_BYPASS;
    publishSnapshot(response != TUNER_RESP_ERROR);
    // A command is unsuccessful if metering could not be restored either.
    if (!switchToMeterMode()) return TUNER_RESP_ERROR;
    return response;
}

bool TunerProtocol::toggleAntenna() {
    const char response = controlResponse(TUNER_CMD_TOGGLE_ANT);
    return response == TUNER_RESP_ANT_A || response == TUNER_RESP_ANT_B;
}

bool TunerProtocol::selectAntenna(tuner_ant_t antenna) {
    if (antenna != ANT_A && antenna != ANT_B) return false;
    TunerLock lock(m_mutex);
    if (!lock) return false;
    // The wire protocol only offers a toggle. Its acknowledgement tells us
    // the selected port, including when the initial port was unknown.
    if (getState().antenna == antenna) return true;
    if (!toggleAntenna()) return false;
    return m_antenna == antenna || (toggleAntenna() && m_antenna == antenna);
}

bool TunerProtocol::memoryTune() {
    const char response = controlResponse(TUNER_CMD_MEM_TUNE);
    return response == TUNER_RESP_GOOD || response == TUNER_RESP_OKAY;
}

bool TunerProtocol::fullTune() {
    const char response = controlResponse(TUNER_CMD_FULL_TUNE);
    return response == TUNER_RESP_GOOD || response == TUNER_RESP_OKAY;
}

bool TunerProtocol::bypass() { return controlResponse(TUNER_CMD_BYPASS) == TUNER_RESP_BYPASS; }
bool TunerProtocol::setAutoMode() { return controlResponse(TUNER_CMD_AUTO_MODE) == TUNER_RESP_AUTO; }
bool TunerProtocol::setManualMode() { return controlResponse(TUNER_CMD_MANUAL_MODE) == TUNER_RESP_MANUAL; }
bool TunerProtocol::sync() {
    // The documented sync response is an opaque status byte, unlike the
    // command-specific ASCII acknowledgements above.
    return controlResponse(TUNER_CMD_SYNC) != TUNER_RESP_ERROR;
}

bool TunerProtocol::startMeter() {
    TunerLock lock(m_mutex);
    bool result = sendCommand(TUNER_CMD_METER_START);
    if (result) {
        m_meterActive = true;
        m_state = State::METER;
    }
    return result;
}

bool TunerProtocol::stopMeter() {
    TunerLock lock(m_mutex);
    bool result = sendCommand(TUNER_CMD_METER_STOP);
    if (result) {
        m_meterActive = false;
        m_state = State::RESPONSE;
    }
    return result;
}

void TunerProtocol::publishSnapshot(bool received, bool meterReceived) {
    portENTER_CRITICAL(&m_snapshotMux);
    if (meterReceived) m_lastMeterMs = millis();
    m_snapshot.meter = m_meterData;
    m_snapshot.meterValid = m_hasMeterData;
    m_snapshot.mode = m_mode;
    m_snapshot.antenna = m_antenna;
    if (received) { m_seenResponse = true; m_lastResponseMs = millis(); }
    portEXIT_CRITICAL(&m_snapshotMux);
}

tuner_state_t TunerProtocol::getState() const {
    portENTER_CRITICAL(&m_snapshotMux);
    tuner_state_t state = m_snapshot;
    const uint32_t meterAge = millis() - m_lastMeterMs;
    const bool connected = m_seenResponse && millis() - m_lastResponseMs <= TELEMETRY_TIMEOUT_MS;
    portEXIT_CRITICAL(&m_snapshotMux);
    state.meterValid = state.meterValid && meterAge <= TELEMETRY_TIMEOUT_MS;
    if (!connected) { state.mode = MODE_UNKNOWN; state.antenna = ANT_UNKNOWN; }
    return state;
}

tuner_mode_t TunerProtocol::getMode() const { return getState().mode; }
tuner_ant_t TunerProtocol::getAntenna() const { return getState().antenna; }

void TunerProtocol::setMeterCallback(meter_callback_t callback) {
    m_meterCallback = callback;
}

bool TunerProtocol::isConnected() const {
    portENTER_CRITICAL(&m_snapshotMux);
    const bool connected = m_seenResponse && millis() - m_lastResponseMs <= TELEMETRY_TIMEOUT_MS;
    portEXIT_CRITICAL(&m_snapshotMux);
    return connected;
}

double TunerProtocol::calculatePower(uint16_t raw, double psuVoltage) {
    double tosquare = (1000.0 * psuVoltage * raw) / (65536.0 * 0.707);
    return (tosquare * tosquare) / 50.0;
}

const char* TunerProtocol::bandToString(tuner_band_t band) {
    switch (band) {
        case BAND_160M: return "160m";
        case BAND_80M:  return "80m";
        case BAND_60M:  return "60m";
        case BAND_40M:  return "40m";
        case BAND_30M:  return "30m";
        case BAND_20M:  return "20m";
        case BAND_17M:  return "17m";
        case BAND_15M:  return "15m";
        case BAND_12M:  return "12m";
        case BAND_10M:  return "10m";
        case BAND_6M:   return "6m";
        default:        return "Unknown";
    }
}

tuner_band_t TunerProtocol::decodeBand(uint16_t indicator) {
    // Band thresholds from reverse-engineered data
    if (indicator >= 8230 && indicator <= 9145) return BAND_160M;
    if (indicator >= 4110 && indicator <= 4710) return BAND_80M;
    if (indicator >= 3060 && indicator <= 3080) return BAND_60M;
    if (indicator >= 2250 && indicator <= 2355) return BAND_40M;
    if (indicator >= 1600 && indicator <= 1630) return BAND_30M;
    if (indicator >= 1140 && indicator <= 1180) return BAND_20M;
    if (indicator >= 900  && indicator <= 915)  return BAND_17M;
    if (indicator >= 766  && indicator <= 782)  return BAND_15M;
    if (indicator >= 655  && indicator <= 662)  return BAND_12M;
    if (indicator >= 550  && indicator <= 590)  return BAND_10M;
    if (indicator >= 320  && indicator <= 360)  return BAND_6M;
    return BAND_UNKNOWN;
}

void TunerProtocol::processMeterByte(uint8_t b) {
    if (m_meterIdx < TUNER_METER_DATA_LEN) {
        m_meterBuf[m_meterIdx++] = b;
    } else if (b == 0x3B) {
        m_eomCount++;
        if (m_eomCount == 2) {
            if (m_meterIdx == TUNER_METER_DATA_LEN) {
                publishMeterData();
            }
            m_meterIdx = 0;
            m_eomCount = 0;
        }
    } else {
        // Protocol error
        m_meterIdx = 0;
        m_eomCount = 0;
        m_state = State::ERROR;
    }
}

void TunerProtocol::publishMeterData() {
    // Data arrives in network byte order (big-endian)
    uint16_t fwd_raw = (m_meterBuf[0] << 8) | m_meterBuf[1];
    uint16_t ref_raw = (m_meterBuf[2] << 8) | m_meterBuf[3];
    uint16_t band_raw = (m_meterBuf[4] << 8) | m_meterBuf[5];

    m_meterData.forward_power_raw = fwd_raw;
    m_meterData.reflected_power_raw = ref_raw;
    m_meterData.band_indicator = band_raw;

    const double voltage = voltageSensor.readVoltage();
    m_meterData.forward_power_watts = calculatePower(fwd_raw, voltage);
    m_meterData.reflected_power_watts = calculatePower(ref_raw, voltage);

    // Calculate SWR
    if (m_meterData.reflected_power_watts > 0.0) {
        double ratio = sqrt(m_meterData.forward_power_watts / m_meterData.reflected_power_watts);
        m_meterData.swr = abs((1.0 + ratio) / (1.0 - ratio));
    } else {
        m_meterData.swr = 1.0;
    }

    m_meterData.band = decodeBand(band_raw);

    m_hasMeterData = true;
    publishSnapshot(true, true);
    if (m_meterCallback) {
        m_meterCallback(&m_meterData);
    }
}

void TunerProtocol::process() {
    if (!m_serial || !m_connected) return;
    // Non-blocking try: if a high-level command (toggle, tune, ...) is mid-
    // flight on another task, skip this tick. Meter bytes stay in UART2's
    // RX FIFO and we'll drain them next call.
    TunerLock lock(m_mutex, 0);
    if (m_mutex && !lock) return;

    while (m_serial->available()) {
        uint8_t b = m_serial->read();

        switch (m_state) {
            case State::RESPONSE:
                if (b >= 0x30) {
                    if (m_waitingForResponse && !m_pendingResponse) {
                        m_pendingResponse = (char)b;
                    }
                } else {
                    if (m_waitingForResponse && !m_pendingResponse) {
                        m_pendingResponse = TUNER_RESP_ERROR;
                    }
                }
                break;

            case State::METER:
                processMeterByte(b);
                break;

            case State::ERROR:
                if (b == 0x3B) {
                    m_eomCount++;
                    if (m_eomCount >= 2) {
                        m_state = State::METER;
                        m_eomCount = 0;
                        m_meterIdx = 0;
                    }
                } else { m_eomCount = 0; }
                break;
        }
    }
}
