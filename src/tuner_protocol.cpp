#include "tuner_protocol.h"
#include "voltage_sensor.h"
#include "config.h"
#include <driver/uart.h>
#include <cmath>
#include <new>

namespace {
constexpr char SYNC_RESPONSE[] = "000000000000000AzAz";
constexpr uint8_t SYNC_ZERO_COUNT = 15;

bool isCommandResponse(uint8_t command, uint8_t response) {
    switch (command) {
        case TUNER_CMD_TOGGLE_ANT:
            return response == TUNER_RESP_ANT_A || response == TUNER_RESP_ANT_B;
        case TUNER_CMD_MEM_TUNE:
        case TUNER_CMD_FULL_TUNE:
            return response == TUNER_RESP_GOOD || response == TUNER_RESP_OKAY ||
                   response == TUNER_RESP_FAIL;
        case TUNER_CMD_BYPASS: return response == TUNER_RESP_BYPASS;
        case TUNER_CMD_AUTO_MODE: return response == TUNER_RESP_AUTO;
        case TUNER_CMD_MANUAL_MODE: return response == TUNER_RESP_MANUAL;
        default: return false;
    }
}

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
    m_snapshot.antennaInit = m_antennas.init();
}

TunerProtocol::~TunerProtocol() {
    if (m_serial) delete m_serial;
    if (m_mutex)  vSemaphoreDelete(m_mutex);
}

bool TunerProtocol::begin(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    m_mutex = xSemaphoreCreateRecursiveMutex();
    m_serial = new (std::nothrow) HardwareSerial(TUNER_UART_NUM);
    if (!m_mutex || !m_serial) return false;
    // S/X deliberately wait 250 ms. At 38400 8N1 up to 960 bytes can arrive
    // then, exceeding HardwareSerial's default 256-byte receive ring.
    if (m_serial->setRxBufferSize(2048) < 2048) return false;
    m_serial->onReceiveError([this](hardwareSerial_error_t error) {
        if (error == UART_NO_ERROR) return;
        portENTER_CRITICAL(&m_snapshotMux);
        switch (error) {
            case UART_BUFFER_FULL_ERROR: ++m_diagnostics.bufferFull; break;
            case UART_FIFO_OVF_ERROR: ++m_diagnostics.fifoOverflow; break;
            case UART_FRAME_ERROR: ++m_diagnostics.frameErrors; break;
            case UART_PARITY_ERROR: ++m_diagnostics.parityErrors; break;
            case UART_BREAK_ERROR: ++m_diagnostics.breaks; break;
            default: break;
        }
        portEXIT_CRITICAL(&m_snapshotMux);
        m_rxErrorPending.store(true);
    });
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

bool TunerProtocol::automaticRelayAllowed() {
    if (!m_automaticRelays) return true;
    const auto radio = m_radioStateGetter ? m_radioStateGetter() : ldg::RadioState{};
    const bool radioGuard = m_preventiveSelection || radio.ownsBand();
    const bool allowed = m_manualRevision.load() == m_relayManualRevision &&
        (!radioGuard || (radio.safeRx() && (!ldg::knownBand(m_relayBand) ||
            (ldg::frequencyBand(radio.txFrequencyHz) == m_relayBand && radio.stableMs >= 200)) &&
            m_hasMeterData && m_antennas.hasFreshMeter(millis()) &&
            m_meterData.forward_power_raw == 0 && m_meterData.forward_power_watts == 0));
    if (!allowed) m_relayBlocked = true;
    return allowed;
}

void TunerProtocol::observeRadio() {
    const auto previous = m_antennas.automatic();
    const auto band = m_antennas.currentBand();
    const bool source = m_antennas.usesRadio();
    const auto radio = m_radioStateGetter ? m_radioStateGetter() : ldg::RadioState{};
    m_tuning.setPtt(radio.connection == ldg::RadioConnection::Ready && radio.rxKnown &&
                    radio.ageMs <= ldg::RadioState::LivenessMs, radio.transmitting);
    m_antennas.observeRadio(radio, m_manualRevision.load());
    if (previous != m_antennas.automatic() || band != m_antennas.currentBand() || source != m_antennas.usesRadio())
        publishSnapshot();
}

bool TunerProtocol::sendCommand(uint8_t cmd) {
    if (!m_serial || !m_connected) return false;

    // LDG specifies at least 200 ms after RECEIVING an acknowledgement. Apply
    // the guard before even waking the tuner; a slow operation cannot consume it.
    if (m_ackGuardActive) {
        const uint32_t elapsed = millis() - m_lastAckMs;
        if (elapsed < TUNER_CMD_DELAY_MS) delay(TUNER_CMD_DELAY_MS - elapsed);
        m_ackGuardActive = false;
    }
    if (!sendWake()) return false;

    // Recheck after the UART guard, wake bytes and synchronization, immediately
    // before EACH physical toggle (including absolute-selection recovery).
    if (cmd == TUNER_CMD_TOGGLE_ANT && !automaticRelayAllowed()) return false;

    if (m_serial->write(cmd) != 1) {
        return false;
    }

    // S/X have no acknowledgement. Give the mode switch and its final meter
    // bytes time to settle. Commands with replies are read immediately below.
    if (cmd == TUNER_CMD_METER_START || cmd == TUNER_CMD_METER_STOP)
        delay(TUNER_CMD_DELAY_MS);

    return true;
}

bool TunerProtocol::switchToControlMode() {
    m_state = State::RESPONSE;
    m_meterIdx = m_eomCount = 0;
    if (!sendCommand(TUNER_CMD_METER_STOP)) return false;
    // Discard the final meter frame that was already in flight before X.
    while (m_serial->available()) m_serial->read();
    // A reported fault in that discarded batch must not reject the next clean
    // command acknowledgement. New faults during the exchange are still fatal.
    m_rxErrorPending.exchange(false);
    m_meterActive = false;
    return true;
}

bool TunerProtocol::switchToMeterMode() {
    m_meterRequested = true;
    m_meterIdx = m_eomCount = 0;
    m_state = State::METER;
    m_meterActive = sendCommand(TUNER_CMD_METER_START);
    m_lastMeterStartMs = millis();
    return m_meterActive;
}

char TunerProtocol::waitForResponse(uint8_t cmd, uint32_t timeoutMs) {
    const uint32_t start = millis();
    m_responseTimeoutMs = timeoutMs;
    m_waitingForResponse = true;
    m_pendingResponse = 0;
    m_expectedCommand = cmd;
    m_syncMatched = 0;
    for (;;) {
        process();
        if (m_pendingResponse != 0) break;
        if (millis() - start >= timeoutMs) break;
        delay(1);
    }
    m_waitingForResponse = false;
    m_responseWaitMs = millis() - start;
    m_timedOutCommand = m_pendingResponse ? 0 : cmd;
    m_expectedCommand = 0;
    return m_pendingResponse ? m_pendingResponse : TUNER_RESP_ERROR;
}

void TunerProtocol::drainLateResponse() {
    if (!m_timedOutCommand) return;
    // Keep the response parser active through the timeout boundary. A reply
    // just after the deadline must not become meter payload or be followed
    // immediately by S. It can confirm state, but not turn a timeout into
    // command success. sendCommand() still enforces the post-ACK guard.
    m_expectedCommand = m_timedOutCommand;
    m_drainingLateResponse = true;
    const uint32_t started = millis();
    while (millis() - started < TUNER_CMD_DELAY_MS) {
        process();
        delay(1);
    }
    m_drainingLateResponse = false;
    m_expectedCommand = m_timedOutCommand = 0;
}

void TunerProtocol::resetCommandTrace(uint8_t exchange) {
    m_exchange = exchange;
    m_exchangeWritten = m_exchangeUartError = false;
    m_commandRxNext = m_commandRxCount = 0;
    m_commandRxBytes = 0;
    m_responseTimeoutMs = m_responseWaitMs = 0;
    m_lateResponse = m_timedOutCommand = 0;
}

char TunerProtocol::exchangeCommand(uint8_t cmd) {
    resetCommandTrace(cmd);
    if (!sendCommand(cmd)) return TUNER_RESP_ERROR;
    m_exchangeWritten = true;
    const uint32_t timeout = (cmd == TUNER_CMD_MEM_TUNE || cmd == TUNER_CMD_FULL_TUNE)
        ? TUNER_TUNE_TIMEOUT_MS
        : cmd == TUNER_CMD_SYNC ? TUNER_SYNC_TIMEOUT_MS
        : cmd == TUNER_CMD_TOGGLE_ANT ? TUNER_ANTENNA_TIMEOUT_MS
        : (cmd == TUNER_CMD_BYPASS || cmd == TUNER_CMD_AUTO_MODE || cmd == TUNER_CMD_MANUAL_MODE)
            ? TUNER_MODE_TIMEOUT_MS : TUNER_RESPONSE_TIMEOUT_MS;
    return waitForResponse(cmd, timeout);
}

char TunerProtocol::controlResponse(uint8_t cmd) {
    TunerLock lock(m_mutex);
    if (!lock) return TUNER_RESP_ERROR;
    const uint32_t started = millis();
    resetCommandTrace(0);
    char response = TUNER_RESP_ERROR;
    if (cmd == TUNER_CMD_TOGGLE_ANT && m_automaticRelays) process();
    if (switchToControlMode()) {
        bool ready = true;
        if (m_needsSync && cmd != TUNER_CMD_SYNC) {
            ready = exchangeCommand(TUNER_CMD_SYNC) == TUNER_CMD_SYNC;
            if (ready) { m_needsSync = false; publishSnapshot(true); }
        }
        if (ready) response = exchangeCommand(cmd);
    }
    drainLateResponse();
    if (!(m_automaticRelays && m_relayBlocked && !m_exchangeWritten)) m_needsSync = response == TUNER_RESP_ERROR;
    const bool commandWritten = m_exchange == cmd && m_exchangeWritten;
    const char stateResponse = response != TUNER_RESP_ERROR ? response :
        m_exchangeUartError ? 0 : char(m_lateResponse);
    if (cmd == TUNER_CMD_TOGGLE_ANT && commandWritten) {
        if (stateResponse == TUNER_RESP_ANT_A) m_antenna = ANT_A;
        else if (stateResponse == TUNER_RESP_ANT_B) m_antenna = ANT_B;
        else m_antenna = ANT_UNKNOWN;
    } else if (cmd == TUNER_CMD_AUTO_MODE) m_mode = response == TUNER_RESP_AUTO ? MODE_AUTO : MODE_UNKNOWN;
    else if (cmd == TUNER_CMD_MANUAL_MODE) m_mode = response == TUNER_RESP_MANUAL ? MODE_MANUAL : MODE_UNKNOWN;
    else if (cmd == TUNER_CMD_BYPASS) m_mode = response == TUNER_RESP_BYPASS ? MODE_BYPASS : MODE_UNKNOWN;
    else if ((cmd == TUNER_CMD_MEM_TUNE || cmd == TUNER_CMD_FULL_TUNE) && m_mode == MODE_BYPASS)
        m_mode = MODE_UNKNOWN; // A tuning cycle may change relay state; no bypass status is returned.
    publishSnapshot(response != TUNER_RESP_ERROR);
    // A command is unsuccessful if metering could not be restored either.
    const bool meterRestored = switchToMeterMode();
    const bool failedTune = (cmd == TUNER_CMD_MEM_TUNE || cmd == TUNER_CMD_FULL_TUNE) &&
                            response == TUNER_RESP_FAIL;
    portENTER_CRITICAL(&m_snapshotMux);
    m_diagnostics.hasCommand = true;
    m_diagnostics.command = cmd;
    m_diagnostics.response = response != TUNER_RESP_ERROR ? uint8_t(response) : 0;
    m_diagnostics.lateResponse = m_lateResponse;
    m_diagnostics.commandSuccess = meterRestored && response != TUNER_RESP_ERROR && !failedTune;
    m_diagnostics.commandDurationMs = millis() - started;
    m_diagnostics.exchange = m_exchange;
    m_diagnostics.exchangeWritten = m_exchangeWritten;
    m_diagnostics.exchangeUartError = m_exchangeUartError;
    m_diagnostics.meterRestarted = meterRestored;
    m_diagnostics.commandRxBytes = m_commandRxBytes;
    m_diagnostics.commandRxCount = m_commandRxCount;
    m_diagnostics.responseTimeoutMs = m_responseTimeoutMs;
    m_diagnostics.responseWaitMs = m_responseWaitMs;
    const unsigned first = m_commandRxCount < sizeof(m_commandRx) ? 0 : m_commandRxNext;
    for (unsigned i = 0; i < m_commandRxCount; ++i)
        m_diagnostics.commandRx[i] = m_commandRx[(first + i) % sizeof(m_commandRx)];
    portEXIT_CRITICAL(&m_snapshotMux);
    if (!meterRestored) { m_needsSync = true; return TUNER_RESP_ERROR; }
    return response;
}

bool TunerProtocol::toggleAntenna() {
    const char response = controlResponse(TUNER_CMD_TOGGLE_ANT);
    return response == TUNER_RESP_ANT_A || response == TUNER_RESP_ANT_B;
}

void TunerProtocol::configureAntennas(const ldg::AntennaSettings& settings) {
    if (m_antennas.configure(settings)) publishSnapshot();
}

void TunerProtocol::configureTuning(const ldg::TuningSettings& settings) {
    if (m_tuning.configure(settings)) {
        m_restoreAutoAfterRf = false;
        publishSnapshot();
    }
}

bool TunerProtocol::serviceTuning(bool manualPending, bool allowTune) {
    using Action = ldg::TuningAutomation::Action;
    if (allowTune && m_tuning.enabled()) process(); // Decode the post-A batch before deciding on T.
    const auto previous = m_tuning.state();
    const bool ready = m_antennas.init() == ldg::AntennaInit::Ready &&
                       !m_antennas.selectionPending() && ldg::knownAntenna(getAntenna());
    const uint32_t revision = m_manualRevision.load();
    const auto action = m_tuning.poll(millis(), getMode(), ready, manualPending, allowTune);
    if (previous != m_tuning.state()) publishSnapshot();
    if (action == Action::None) return false;
    // A queued explicit command takes priority over any unstarted policy action.
    if (revision != m_manualRevision.load()) { m_tuning.manualStarted(); publishSnapshot(); return false; }
    bool success = false;
    if (action == Action::MemoryTune) {
        success = memoryTune(); // One cycle per RF start or confirmed new SWR rise; no retry loop.
        m_tuning.memoryComplete(success, millis());
    } else {
        for (unsigned i = 0; !success && i < 2; ++i) {
            if (revision != m_manualRevision.load()) break;
            if (i && action == Action::Auto && m_tuning.enabled()) {
                process();
                if (!m_tuning.allowsRfCommand(millis())) break;
            }
            success = action == Action::Manual ? setManualMode() : setAutoMode();
        }
        m_tuning.modeComplete(action, success);
    }
    process(); // Post-command RF/SWR is a new observation, never the pre-T sample.
    publishSnapshot();
    return true;
}

void TunerProtocol::manualCommandStarted() {
    m_tuning.manualStarted();
    m_initialModeHandled = true;
    m_restoreAutoAfterRf = false; // A user's next mode choice takes priority.
    m_probeRestore = ANT_UNKNOWN;
    m_antennas.manualStarted();
    publishSnapshot();
}

void TunerProtocol::manualCommandComplete(uint8_t command, bool success) {
    if (command == TUNER_CMD_SELECT_ANT_A || command == TUNER_CMD_SELECT_ANT_B ||
        command == TUNER_CMD_TOGGLE_ANT)
        m_antennas.manualAntennaComplete(success && getAntenna() != ANT_UNKNOWN);
    publishSnapshot();
}

void TunerProtocol::serviceAntennas(bool manualPending) {
    const uint32_t revision = m_manualRevision.load();
    observeRadio();
    // A written S is not proof that streaming resumed. Recover from a lost
    // meter-start even before another band has been observed. Only actual
    // decoded frames reset the silence timer; neither RX noise nor status
    // requests can create zero power or an antenna transition.
    const uint32_t now = millis();
    if (!manualPending && m_meterRequested &&
        now - m_lastMeterStartMs >= METER_RESTART_INTERVAL_MS &&
        (!m_hasMeterData || now - m_lastMeterMs >= METER_RESTART_INTERVAL_MS)) {
        portENTER_CRITICAL(&m_snapshotMux);
        ++m_diagnostics.meterRestarts;
        portEXIT_CRITICAL(&m_snapshotMux);
        startMeter();
        process();
    }
    if (serviceTuning(manualPending, false)) return;
    if (!m_tuning.enabled() && !m_initialModeHandled && !manualPending && m_antennas.enabled() &&
        m_antennas.init() == ldg::AntennaInit::Ready) {
        m_initialModeHandled = true;
        // With band automation enabled, establish AUTO after the startup
        // antenna check. LDG has no mode query. An explicit manual command
        // suppresses this one startup policy write.
        if (getMode() == MODE_UNKNOWN) {
            bool confirmed = false;
            for (unsigned i = 0; !confirmed && i < 2; ++i) confirmed = setAutoMode();
            m_restoreAutoAfterRf = !confirmed;
            publishSnapshot();
            return;
        }
    }
    if (!m_tuning.enabled() && m_restoreAutoAfterRf && !m_antennas.waitingTuner() && !manualPending && m_antennas.hasFreshMeter(millis())) {
        const auto state = getState();
        if (state.meterValid && state.meter.forward_power_raw == 0 &&
            state.meter.forward_power_watts == 0) {
            // Both in-RF restore attempts lost their ACKs. Try once after a
            // real zero-power packet; never keep retrying in an RF pause.
            m_restoreAutoAfterRf = false;
            const bool restored = setAutoMode();
            portENTER_CRITICAL(&m_snapshotMux);
            ++m_diagnostics.antennaSelection.restoreAttempts;
            m_diagnostics.antennaSelection.autoRestored = restored;
            portEXIT_CRITICAL(&m_snapshotMux);
        }
    }
    if (ldg::knownAntenna(m_probeRestore)) {
        const auto radio = m_radioStateGetter ? m_radioStateGetter() : ldg::RadioState{};
        if (manualPending || !m_antennas.hasFreshMeter(millis()) || m_meterData.forward_power_raw != 0 ||
            m_meterData.forward_power_watts != 0 || (radio.ownsBand() && !radio.safeRx())) return;
        m_automaticRelays = true; m_preventiveSelection = radio.ownsBand(); m_relayBlocked = false;
        m_relayManualRevision = m_manualRevision.load(); m_relayBand = BAND_UNKNOWN;
        const bool restored = selectAntenna(m_probeRestore);
        m_automaticRelays = false;
        if (!m_relayBlocked) { m_probeRestore = ANT_UNKNOWN; m_antennas.probeComplete(restored); }
        publishSnapshot(); return;
    }
    const auto previousInit = m_antennas.init();
    const auto previousAuto = m_antennas.automatic();
    const auto request = m_antennas.poll(millis(), m_manualRevision.load(), manualPending,
                                       getAntenna(), getMode() == MODE_UNKNOWN);
    if (previousInit != m_antennas.init() || previousAuto != m_antennas.automatic()) publishSnapshot();
    if (request.action == ldg::AntennaAutomation::Action::Probe) {
        m_automaticRelays = true; m_preventiveSelection = m_antennas.usesRadio(); m_relayBlocked = false;
        m_relayManualRevision = m_manualRevision.load(); m_relayBand = BAND_UNKNOWN;
        // One started operation: finish restoring before a queued manual
        // command runs. Missing ACKs never trigger a blind retry.
        bool success = toggleAntenna();
        if (success) {
            const tuner_ant_t original = getAntenna() == ANT_A ? ANT_B : ANT_A;
            m_antennas.restoring();
            publishSnapshot();
            success = toggleAntenna() && getAntenna() == original;
            if (!success && m_relayBlocked) m_probeRestore = original;
        }
        m_automaticRelays = false;
        if (!ldg::knownAntenna(m_probeRestore)) {
            if (m_relayBlocked) m_antennas.probeDeferred();
            else m_antennas.probeComplete(success);
        }
        publishSnapshot();
    } else if (request.action == ldg::AntennaAutomation::Action::Select) {
        selectBandAntenna(request.antenna, request.recovery);
        publishSnapshot();
    } else if (request.action == ldg::AntennaAutomation::Action::RefreshMeter) {
        if (!startMeter()) m_antennas.meterRefreshFailed();
        else process();
        publishSnapshot();
    }
    if (revision != m_manualRevision.load()) {
        m_tuning.manualStarted(); // A command queued during A cancels the unstarted T.
        publishSnapshot();
        return;
    }
    serviceTuning(manualPending, true);
}

void TunerProtocol::selectBandAntenna(tuner_ant_t antenna, uint8_t recovery) {
    const uint32_t started = millis();
    const tuner_band_t band = m_antennas.currentBand();
    const uint32_t manualRevision = m_manualRevision.load();
    const bool preventive = m_antennas.usesRadio();
    m_automaticRelays = true; m_preventiveSelection = preventive; m_relayBlocked = false;
    m_relayManualRevision = manualRevision; m_relayBand = band;
    uint32_t framesBeforeSelection = 0;
    ldg::AntennaSelectionDiagnostics trace;
    trace.present = true;
    trace.target = antenna;
    trace.recovery = recovery;
    trace.modeGuard = getMode() == MODE_AUTO || recovery != 0;
    const bool restoreRequired = trace.modeGuard && !m_tuning.enabled();
    // AT-1000ProII can ignore A during RF in AUTO. M is idempotent; require
    // its ACK before touching the antenna and restore the known original mode.
    // Mode writes are idempotent. One retry may recover a lost mode ACK;
    // controlResponse() synchronizes before retransmitting after a timeout.
    for (unsigned i = 0; trace.modeGuard && !trace.manualConfirmed && i < 2; ++i) {
        ++trace.manualAttempts;
        trace.manualConfirmed = setManualMode();
    }
    const bool prepared = !trace.modeGuard || trace.manualConfirmed;
    if (!prepared) trace.deferred = m_antennas.selectionDeferred(band, started);
    bool fresh = false, valid = false, lowPower = false;
    for (unsigned attempt = 0; prepared && attempt < 2; ++attempt) {
        // M restores metering. Decode that fresh batch before starting A,
        // rather than using the pre-M power or a held display reading.
        const uint32_t meterWaitAt = millis();
        do {
            process();
            if (!m_meterActive || m_antennas.hasFreshMeter(millis())) break;
            delay(1);
        } while (millis() - meterWaitAt < ldg::AntennaAutomation::MeterWaitMs);
        const auto state = getState();
        fresh = state.meterValid && m_meterActive && m_antennas.hasFreshMeter(millis());
        const double power = state.meter.forward_power_watts;
        valid = fresh && std::isfinite(power) && power >= 0;
        lowPower = valid && (preventive ? state.meter.forward_power_raw == 0 && power == 0 : power < 100);
        const bool sameBand = state.meter.forward_power_raw == 0 || state.meter.band == band;
        if (lowPower && sameBand) {
            if (attempt) {
                m_antennas.selectionRetrying();
                publishSnapshot();
            }
            trace.selectionAttempted = true;
            ++trace.attempts;
            framesBeforeSelection = m_diagnostics.frames;
            trace.selectionConfirmed = selectAntenna(antenna);
            trace.response = getDiagnostics().response;
        } else break;
        if (trace.selectionConfirmed) break;
        // Retry the named selection, not a blind A toggle: sync first and
        // use each ACK to reach the requested port if the lost ACK hid a switch.
        // Fresh power/band are checked again before this one recovery attempt.
    }
    // AUTO may have tuned the old antenna before M/A completed. Restoring C
    // alone does not start a new cycle on AT-1000ProII. Tune the confirmed new
    // port once while still in MANUAL, then restore AUTO even if T fails.
    if (restoreRequired && trace.selectionConfirmed && !preventive)
        retuneBandAntenna(band, manualRevision, framesBeforeSelection, trace);
    // A missing M ACK can mean M took effect. Keep AUTO restoration pending
    // with the saved choice, rather than restarting the old antenna's search.
    for (unsigned i = 0; restoreRequired && !trace.deferred && !trace.autoRestored && i < 2; ++i) {
        ++trace.restoreAttempts;
        trace.autoRestored = setAutoMode();
    }
    m_restoreAutoAfterRf = restoreRequired && !trace.autoRestored;
    m_automaticRelays = false;
    if (preventive && !trace.selectionConfirmed &&
        (m_relayBlocked || (prepared && !lowPower)) && m_manualRevision.load() == manualRevision) {
        m_antennas.deferRadioSelection();
        trace.deferred = true;
    }
    if (!trace.deferred) {
        if (!prepared || (restoreRequired && !trace.autoRestored)) m_antennas.selectionComplete(false);
        else if (!lowPower) m_antennas.selectionPowerChanged(valid);
        else m_antennas.selectionComplete(trace.selectionConfirmed);
    }
    if (trace.selectionConfirmed) m_tuning.antennaSelected();
    if (trace.selectionConfirmed && (!restoreRequired || trace.autoRestored)) {
        using Result = ldg::MemoryTuneResult;
        using Status = ldg::AntennaAuto;
        switch (trace.memoryTune) {
            case Result::Failed: m_antennas.selectionTuneStatus(Status::TuneFailed); break;
            case Result::NoRf: m_antennas.selectionTuneStatus(Status::TuneNoRf); break;
            case Result::HighPower: m_antennas.selectionTuneStatus(Status::TuneHighPower); break;
            case Result::InvalidPower: m_antennas.selectionTuneStatus(Status::TuneInvalidPower); break;
            case Result::BandChanged: m_antennas.selectionTuneStatus(Status::TuneBandChanged); break;
            case Result::ManualOverride: m_antennas.selectionTuneStatus(Status::TuneManual); break;
            default: break;
        }
    }
    trace.durationMs = millis() - started;
    portENTER_CRITICAL(&m_snapshotMux);
    m_diagnostics.antennaSelection = trace;
    portEXIT_CRITICAL(&m_snapshotMux);
}

void TunerProtocol::retuneBandAntenna(tuner_band_t band, uint32_t manualRevision,
                                    uint32_t previousFrames, ldg::AntennaSelectionDiagnostics& trace) {
    using Result = ldg::MemoryTuneResult;
    const uint32_t started = millis();
    // Require an actual post-selection packet: the previous antenna's power
    // and held display readings cannot authorize tuning the new load.
    bool fresh = false;
    do {
        process();
        fresh = m_diagnostics.frames != previousFrames && m_meterActive &&
                m_antennas.hasFreshMeter(millis());
        if (fresh || !m_meterActive || m_manualRevision.load() != manualRevision) break;
        delay(1);
    } while (millis() - started < ldg::AntennaAutomation::MeterWaitMs);
    const auto state = getState();
    const double power = state.meter.forward_power_watts;
    if (m_manualRevision.load() != manualRevision) trace.memoryTune = Result::ManualOverride;
    else if (!fresh || !state.meterValid || !std::isfinite(power) || power < 0) trace.memoryTune = Result::InvalidPower;
    else if (power >= 100) trace.memoryTune = Result::HighPower;
    else if (!state.meter.forward_power_raw || power == 0) trace.memoryTune = Result::NoRf;
    else if (state.meter.band != band) trace.memoryTune = Result::BandChanged;
    else {
        trace.memoryTune = Result::Tuning;
        m_antennas.selectionTuneStatus(ldg::AntennaAuto::Tuning);
        portENTER_CRITICAL(&m_snapshotMux);
        m_diagnostics.antennaSelection = trace;
        portEXIT_CRITICAL(&m_snapshotMux);
        publishSnapshot();
        const bool success = memoryTune(); // One cycle, no timeout/failure loop.
        trace.tuneResponse = getDiagnostics().response;
        trace.memoryTune = success ? Result::Succeeded : Result::Failed;
    }
}

bool TunerProtocol::selectAntenna(tuner_ant_t antenna) {
    if (antenna != ANT_A && antenna != ANT_B) return false;
    TunerLock lock(m_mutex);
    if (!lock) return false;
    // The wire protocol only offers a toggle. Its acknowledgement tells us
    // the selected port, including when the initial port was unknown.
    if (getState().antenna == antenna) {
        // A known port needs no relay toggle, but a failed meter restart must
        // still be recovered before reporting a successful operation.
        return (!m_needsSync && isConnected()) || sync();
    }
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
bool TunerProtocol::setControlledMode() {
    if (!m_tuning.enabled()) return false;
    m_tuning.controlledSelected();
    publishSnapshot();
    const bool success = setManualMode();
    m_tuning.modeComplete(ldg::TuningAutomation::Action::Manual, success);
    publishSnapshot();
    return success;
}
bool TunerProtocol::sync() {
    return controlResponse(TUNER_CMD_SYNC) == TUNER_CMD_SYNC;
}

bool TunerProtocol::startMeter() {
    TunerLock lock(m_mutex);
    if (!lock) return false;
    return switchToMeterMode();
}

bool TunerProtocol::stopMeter() {
    TunerLock lock(m_mutex);
    if (!lock) return false;
    m_meterRequested = false;
    return switchToControlMode();
}

void TunerProtocol::publishSnapshot(bool received, bool meterReceived) {
    portENTER_CRITICAL(&m_snapshotMux);
    if (meterReceived) {
        m_lastMeterMs = millis();
        if (ldg::knownBand(m_meterData.band)) m_snapshot.lastBand = m_meterData.band;
    }
    m_snapshot.meter = m_meterData;
    m_snapshot.meterValid = m_hasMeterData;
    m_snapshot.mode = m_mode;
    m_snapshot.antenna = m_antenna;
    if (m_antenna != ANT_UNKNOWN) m_snapshot.lastAntenna = m_antenna;
    m_snapshot.tuningManaged = m_tuning.enabled();
    m_snapshot.tuningState = m_tuning.state();
    m_snapshot.antennaInit = m_antennas.init();
    m_snapshot.antennaAuto = m_antennas.automatic();
    m_snapshot.antennaAutoBand = m_antennas.currentBand();
    m_snapshot.antennaFromRadio = m_antennas.usesRadio();
    if (received) { m_seenResponse = true; m_lastResponseMs = millis(); }
    portEXIT_CRITICAL(&m_snapshotMux);
}

tuner_state_t TunerProtocol::getState() const {
    portENTER_CRITICAL(&m_snapshotMux);
    tuner_state_t state = m_snapshot;
    const uint32_t meterAge = millis() - m_lastMeterMs;
    portEXIT_CRITICAL(&m_snapshotMux);
    state.meterAgeMs = state.meterValid ? meterAge : UINT32_MAX;
    state.meterValid = state.meterValid && meterAge <= TELEMETRY_TIMEOUT_MS;
    if (m_radioStateGetter) state.radio = m_radioStateGetter();
    // Meter silence is not evidence that a relay or AUTO/MANUAL setting changed.
    // Keep the last acknowledged controls; connectivity has its own accessor.
    return state;
}

ldg::TunerDiagnostics TunerProtocol::getDiagnostics() const {
    portENTER_CRITICAL(&m_snapshotMux);
    ldg::TunerDiagnostics result = m_diagnostics;
    const uint8_t next = m_rxTailNext;
    portEXIT_CRITICAL(&m_snapshotMux);
    // Return the ring in wire order, including when it wrapped mid-frame.
    if (result.rxTailCount == sizeof(result.rxTail)) {
        uint8_t ring[sizeof(result.rxTail)];
        memcpy(ring, result.rxTail, sizeof(ring));
        for (unsigned i = 0; i < sizeof(result.rxTail); ++i)
            result.rxTail[i] = ring[(next + i) % sizeof(ring)];
    }
    return result;
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
        portENTER_CRITICAL(&m_snapshotMux);
        ++m_diagnostics.framingErrors;
        portEXIT_CRITICAL(&m_snapshotMux);
        m_antennas.invalidateFrame();
        m_tuning.invalidateFrame();
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

    // Both raw readings share the same square-law conversion, so their ratio
    // is the reflection coefficient. Without forward RF, or with reflected RF
    // >= forward RF, a finite SWR would be misleading. NaN becomes JSON null.
    const double forward = m_meterData.forward_power_watts;
    const double reflected = m_meterData.reflected_power_watts;
    m_meterData.swr = NAN;
    if (std::isfinite(forward) && std::isfinite(reflected) && forward > 0.0 &&
        ref_raw < fwd_raw) {
        const double gamma = double(ref_raw) / double(fwd_raw);
        m_meterData.swr = (1.0 + gamma) / (1.0 - gamma);
    }

    m_meterData.band = decodeBand(band_raw);

    portENTER_CRITICAL(&m_snapshotMux);
    ++m_diagnostics.frames;
    m_diagnostics.hasFrame = true;
    m_diagnostics.lastFrameMs = millis();
    m_diagnostics.psuVoltage = voltage;
    memcpy(m_diagnostics.lastFrame, m_meterBuf, TUNER_METER_DATA_LEN);
    m_diagnostics.lastFrame[6] = m_diagnostics.lastFrame[7] = 0x3b;
    portEXIT_CRITICAL(&m_snapshotMux);
    m_hasMeterData = true;
    m_antennas.observe(m_meterData, millis(), m_manualRevision.load());
    m_tuning.observe(m_meterData, millis());
    publishSnapshot(true, true);
    if (m_meterCallback) {
        m_meterCallback(&m_meterData);
    }
}

void TunerProtocol::processResponseByte(uint8_t b) {
    if (m_drainingLateResponse ? m_lateResponse != 0 : !m_waitingForResponse || m_pendingResponse) return;
    if (m_expectedCommand == TUNER_CMD_SYNC) {
        // Search for the whole signature across reads, ignoring noise. Extra
        // leading zeroes retain the last 15 zeroes as a possible prefix.
        if (b == uint8_t(SYNC_RESPONSE[m_syncMatched])) {
            ++m_syncMatched;
        } else {
            m_syncMatched = b == '0' ? (m_syncMatched == SYNC_ZERO_COUNT ? SYNC_ZERO_COUNT : 1) : 0;
        }
        if (m_syncMatched != sizeof(SYNC_RESPONSE) - 1) return;
        if (m_drainingLateResponse) m_lateResponse = TUNER_CMD_SYNC;
        else m_pendingResponse = TUNER_CMD_SYNC; // Internal completion marker, not a wire ACK.
    } else {
        if (!isCommandResponse(m_expectedCommand, b)) return;
        if (m_drainingLateResponse) m_lateResponse = b;
        else m_pendingResponse = char(b);
    }
    m_lastAckMs = millis();
    m_ackGuardActive = true;
}

void TunerProtocol::process() {
    if (!m_serial || !m_connected) return;
    // Non-blocking try: if a high-level command (toggle, tune, ...) is mid-
    // flight on another task, skip this tick. Meter bytes stay in UART2's
    // RX FIFO and we'll drain them next call.
    TunerLock lock(m_mutex, 0);
    if (m_mutex && !lock) return;
    observeRadio();

    uint8_t tail[32];
    unsigned count = 0;
    bool discard = false;
    for (;;) {
        const int available = m_serial->available();
        if (m_rxErrorPending.exchange(false)) {
            // The event does not identify the damaged byte in the RX ring.
            // Drain this batch, then find a new frame boundary. Never turn an
            // acknowledgement from a damaged batch into command success.
            discard = true;
            m_antennas.invalidateFrame();
            m_tuning.invalidateFrame();
            if (m_waitingForResponse || m_drainingLateResponse) m_exchangeUartError = true;
            m_meterIdx = m_eomCount = 0;
            if (m_state != State::RESPONSE) m_state = State::ERROR;
            else if (m_waitingForResponse) m_pendingResponse = TUNER_RESP_ERROR;
        }
        if (available <= 0) break;
        const int value = m_serial->read();
        if (value < 0) break;
        const uint8_t b = uint8_t(value);
        tail[count++ % sizeof(tail)] = b;
        if (m_waitingForResponse || m_drainingLateResponse) {
            m_commandRx[m_commandRxNext] = b;
            m_commandRxNext = (m_commandRxNext + 1) % sizeof(m_commandRx);
            if (m_commandRxCount < sizeof(m_commandRx)) ++m_commandRxCount;
            ++m_commandRxBytes;
        }
        if (discard) continue;

        switch (m_state) {
            case State::RESPONSE:
                processResponseByte(b);
                break;

            case State::METER:
                processMeterByte(b);
                break;

            case State::ERROR:
                if (b == 0x3B) {
                    m_eomCount++;
                    if (m_eomCount >= 2) {
                        m_state = State::METER;
                        portENTER_CRITICAL(&m_snapshotMux);
                        ++m_diagnostics.resyncs;
                        portEXIT_CRITICAL(&m_snapshotMux);
                        m_eomCount = 0;
                        m_meterIdx = 0;
                    }
                } else { m_eomCount = 0; }
                break;
        }
    }
    if (count) {
        const unsigned kept = count < sizeof(tail) ? count : sizeof(tail);
        portENTER_CRITICAL(&m_snapshotMux);
        m_diagnostics.rxBytes += count;
        for (unsigned i = count - kept; i < count; ++i) {
            m_diagnostics.rxTail[m_rxTailNext] = tail[i % sizeof(tail)];
            m_rxTailNext = (m_rxTailNext + 1) % sizeof(m_diagnostics.rxTail);
            if (m_diagnostics.rxTailCount < sizeof(m_diagnostics.rxTail))
                ++m_diagnostics.rxTailCount;
        }
        portEXIT_CRITICAL(&m_snapshotMux);
    }
}
