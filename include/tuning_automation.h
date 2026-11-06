#pragma once
#include "tuning_settings.h"
#include "tuner_types.h"
namespace ldg {
// UART-worker-owned policy. Only decoded RF samples can start/end a session.
class TuningAutomation {
public:
    static constexpr uint32_t FreshMs = 500, RfOffMs = 1500, SwrWaitMs = 1500, OffSampleGapMs = 3000;
    enum class Action { None, Manual, MemoryTune, Auto };
    bool configure(const TuningSettings& value) {
        if (sameTuningSettings(m_settings, value)) return false;
        const bool modeChanged = m_settings.managed != value.managed;
        m_settings = value;
        if (!modeChanged) {
            m_swrSamples = m_lowSwrSamples = 0;
            if (m_memoryDone && m_session && !m_override && !m_powerBlocked) {
                m_monitorSwr = true;
                if (!m_autoAttempted) m_postMemory = true;
                m_afterFrames = m_frames; // Recheck using observations after the option change.
                m_swrAt = m_meterAt;
                m_state = m_autoActive ? TuningState::AutoActive : TuningState::WaitingSwr;
            }
            return true; // Saving options does not create a new RF session.
        }
        m_session = m_pending = m_monitorSwr = m_autoActive = m_override = m_memoryDone = false;
        m_postMemory = m_cycleArmed = false;
        m_swrSamples = m_lowSwrSamples = 0;
        m_needManual = value.managed;
        m_modeAttempted = m_autoAttempted = false;
        m_positive = 0; m_zeroSeen = m_powerBlocked = m_startHigh = false;
        m_state = value.managed ? TuningState::WaitingManual : TuningState::Native;
        return true;
    }
    void setPtt(bool known, bool transmitting) { m_pttKnown = known; m_pttTx = transmitting; }
    void observe(const tuner_meter_t& meter, uint32_t now) {
        const bool consecutive = m_hasMeter && now - m_meterAt <= FreshMs;
        const bool sameBand = m_hasMeter && meter.band == m_meter.band;
        const bool continuousZeroSamples = m_hasMeter && now - m_meterAt <= OffSampleGapMs;
        m_hasMeter = true; m_meterAt = now; m_meter = meter; ++m_frames;
        if (!m_settings.managed) return;
        const bool valid = std::isfinite(meter.forward_power_watts) && meter.forward_power_watts >= 0;
        const bool rf = valid && meter.forward_power_raw > 0 && meter.forward_power_watts > 0;
        const bool zero = valid && meter.forward_power_raw == 0 && meter.forward_power_watts == 0;
        if (!consecutive) { m_startHigh = false; m_swrSamples = m_lowSwrSamples = 0; }
        // Idle telemetry may arrive in batches after S. Only new zero packets
        // advance RF-off evidence; a disconnected stream cannot end a session.
        if (!continuousZeroSamples) m_zeroSeen = false;
        if (m_monitorSwr) {
            const bool usableSwr = rf && meter.forward_power_watts < 100 &&
                (!m_pttKnown || m_pttTx) && std::isfinite(meter.swr) && meter.swr >= 1;
            if (usableSwr) m_swrAt = now;
            if (usableSwr && meter.swr >= m_settings.swrTenths / 10.0) {
                m_lowSwrSamples = 0;
                if (m_swrSamples < 3) ++m_swrSamples;
            } else {
                m_swrSamples = 0;
                if (usableSwr) {
                    if (m_lowSwrSamples < 3) ++m_lowSwrSamples;
                    if (m_lowSwrSamples >= 3) { m_cycleArmed = true; m_postMemory = false; }
                } else m_lowSwrSamples = 0;
            }
        }
        if (rf && (!m_pttKnown || m_pttTx)) {
            m_zeroSeen = false;
            // RF can appear before LDG has a valid band. Confirm one band over
            // three packets; live PTT supplies TX evidence independently of LDG.
            const bool bandKnown = m_pttKnown || knownBand(meter.band);
            if (!m_session && (!consecutive || (!m_pttKnown && (!sameBand || !bandKnown)))) {
                m_positive = 0; m_startHigh = false;
            }
            if (!m_session && meter.forward_power_watts >= 100) m_startHigh = true;
            if (!m_session && bandKnown && ++m_positive >= 3) {
                m_session = true; m_pending = !m_startHigh; m_override = false;
                m_memoryDone = m_postMemory = m_cycleArmed = false;
                m_powerBlocked = m_startHigh;
                m_autoAttempted = m_modeAttempted = false; m_needManual = true;
                m_state = m_powerBlocked ? TuningState::HighPower : TuningState::WaitingAntenna;
            }
            if (m_session && meter.forward_power_watts >= 100 && !m_autoActive) {
                m_powerBlocked = true;
                m_pending = m_monitorSwr = m_cycleArmed = false; m_state = TuningState::HighPower;
            }
        } else {
            m_positive = 0; m_startHigh = false;
            if (zero && (!m_pttKnown || !m_pttTx)) {
                if (!m_zeroSeen) { m_zeroSeen = true; m_zeroAt = now; }
                if (m_session && (m_pttKnown || now - m_zeroAt >= RfOffMs)) {
                    m_session = m_pending = m_monitorSwr = m_autoActive = m_override = m_memoryDone = false;
                    m_postMemory = m_cycleArmed = false;
                    m_needManual = true; m_modeAttempted = false;
                    m_state = TuningState::ReturningManual;
                }
            } else m_zeroSeen = false;
        }
    }
    Action poll(uint32_t now, tuner_mode_t mode, bool antennaReady, bool manualPending, bool allowTune) {
        const bool fresh = m_hasMeter && now - m_meterAt <= FreshMs;
        const bool valid = fresh && std::isfinite(m_meter.forward_power_watts) && m_meter.forward_power_watts >= 0;
        const bool lowRf = valid && m_meter.forward_power_raw && m_meter.forward_power_watts > 0 &&
                           m_meter.forward_power_watts < 100 && (!m_pttKnown || m_pttTx);
        if (manualPending) return Action::None;
        if (!enabled()) return Action::None;
        if (m_override) return Action::None;
        if (m_needManual) {
            if (mode == MODE_MANUAL) { m_needManual = false; m_modeAttempted = false; }
            else if (valid && m_meter.forward_power_watts < 100 && !m_modeAttempted) {
                m_modeAttempted = true;
                if (m_state != TuningState::ReturningManual) m_state = TuningState::WaitingManual;
                return Action::Manual;
            } else return Action::None;
        }
        if (!m_session) { m_state = TuningState::WaitingRf; return Action::None; }
        if (m_pending && allowTune) {
            if (!antennaReady) { m_state = TuningState::WaitingAntenna; return Action::None; }
            if (!lowRf || mode != MODE_MANUAL) return Action::None;
            m_pending = false; m_state = TuningState::MemoryTune; m_afterFrames = m_frames;
            return Action::MemoryTune;
        }
        if (m_monitorSwr && allowTune) {
            if (!antennaReady) { m_state = TuningState::WaitingAntenna; return Action::None; }
            if (m_frames != m_afterFrames && lowRf && std::isfinite(m_meter.swr) && m_meter.swr >= 1) {
                if (m_swrSamples >= 3 && m_cycleArmed) {
                    // A new confirmed low -> high SWR transition starts another
                    // memory/optional-AUTO cycle, regardless of antenna or band.
                    m_cycleArmed = m_postMemory = m_monitorSwr = false;
                    m_swrSamples = m_lowSwrSamples = 0;
                    m_pending = true; m_needManual = true; m_modeAttempted = false;
                    if (mode != MODE_MANUAL) {
                        m_modeAttempted = true; m_state = TuningState::WaitingManual;
                        return Action::Manual;
                    }
                    m_pending = m_needManual = false;
                    m_state = TuningState::MemoryTune; m_afterFrames = m_frames;
                    return Action::MemoryTune;
                }
                if (m_swrSamples >= 3 && m_postMemory) {
                    m_postMemory = false; // Constant mismatch never loops memory cycles.
                    if (m_settings.autoFallback && !m_autoAttempted && mode == MODE_MANUAL) {
                        m_autoAttempted = true;
                        return Action::Auto;
                    }
                }
                if (m_autoActive) m_state = TuningState::AutoActive;
                else if (m_meter.swr < m_settings.swrTenths / 10.0) m_state = TuningState::Matched;
                else if (m_postMemory) m_state = TuningState::WaitingSwr;
                else if (!m_settings.autoFallback) m_state = TuningState::Matched;
            }
            if (m_monitorSwr && now - m_swrAt >= SwrWaitMs) {
                m_state = TuningState::NoFreshData; // Resume when fresh RF/SWR returns.
            }
        }
        return Action::None;
    }
    void modeComplete(Action action, bool success) {
        if (!enabled()) return;
        if (action == Action::Manual) {
            if (success) {
                m_needManual = false; m_autoActive = false;
                m_state = !m_session ? TuningState::WaitingRf :
                          m_powerBlocked ? TuningState::HighPower : TuningState::WaitingAntenna;
            }
            else {
                m_pending = false;
                m_monitorSwr = m_memoryDone && m_session && !m_override && !m_powerBlocked;
                if (m_monitorSwr) m_needManual = false; // Another genuine SWR rise may retry, not a timer.
                m_autoActive = false;
                m_swrSamples = m_lowSwrSamples = 0;
                m_state = TuningState::Error;
            }
        } else if (action == Action::Auto) {
            m_autoActive = success;
            m_state = success ? TuningState::AutoActive : TuningState::Error;
        }
    }
    void memoryComplete(bool success, uint32_t now) {
        m_swrAt = now;
        m_memoryDone = true;
        m_afterFrames = m_frames;
        m_swrSamples = m_lowSwrSamples = 0;
        m_cycleArmed = false; m_postMemory = true; m_autoAttempted = false;
        m_monitorSwr = m_session && !m_override && !m_powerBlocked;
        m_state = success ? TuningState::WaitingSwr : TuningState::Error;
    }
    void antennaSelected() {
        if (enabled() && m_session && !m_override && !m_powerBlocked) {
            m_pending = true; m_monitorSwr = m_autoActive = m_memoryDone = false; m_autoAttempted = false;
            m_postMemory = m_cycleArmed = false;
            m_state = TuningState::WaitingAntenna;
        }
    }
    void controlledSelected() {
        m_override = false; m_needManual = true; m_modeAttempted = true;
        m_autoActive = m_memoryDone = m_postMemory = m_cycleArmed = false;
        m_state = TuningState::WaitingManual;
    }
    void manualStarted() {
        m_pending = m_monitorSwr = m_postMemory = m_cycleArmed = false;
        if (enabled()) { m_override = true; m_state = TuningState::ManualOverride; }
    }
    void invalidateFrame() { m_hasMeter = false; m_positive = 0; m_zeroSeen = m_startHigh = false; m_swrSamples = m_lowSwrSamples = 0; }
    bool allowsRfCommand(uint32_t now) const {
        return m_hasMeter && now - m_meterAt <= FreshMs && m_session && !m_override && !m_powerBlocked &&
            m_meter.forward_power_raw && std::isfinite(m_meter.forward_power_watts) &&
            m_meter.forward_power_watts > 0 && m_meter.forward_power_watts < 100 && (!m_pttKnown || m_pttTx);
    }
    bool enabled() const { return m_settings.managed; }
    TuningState state() const { return m_state; }
private:
    TuningSettings m_settings{};
    TuningState m_state = TuningState::Native;
    tuner_meter_t m_meter{};
    bool m_hasMeter = false, m_session = false, m_pending = false, m_monitorSwr = false;
    bool m_memoryDone = false, m_postMemory = false, m_cycleArmed = false;
    bool m_needManual = false, m_modeAttempted = false, m_autoAttempted = false, m_autoActive = false;
    bool m_override = false, m_zeroSeen = false, m_pttKnown = false, m_pttTx = false;
    bool m_powerBlocked = false, m_startHigh = false;
    uint8_t m_positive = 0, m_swrSamples = 0, m_lowSwrSamples = 0;
    uint32_t m_meterAt = 0, m_zeroAt = 0, m_swrAt = 0, m_frames = 0, m_afterFrames = 0;
};
}
