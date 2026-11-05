#pragma once
#include "tuning_settings.h"
#include "tuner_types.h"
namespace ldg {
// UART-worker-owned policy. Only decoded RF samples can start/end a session.
class TuningAutomation {
public:
    static constexpr uint32_t FreshMs = 500, RfOffMs = 1500, SwrWaitMs = 1500;
    enum class Action { None, Manual, MemoryTune, Auto };
    bool configure(const TuningSettings& value) {
        if (sameTuningSettings(m_settings, value)) return false;
        const bool modeChanged = m_settings.managed != value.managed;
        m_settings = value;
        if (!modeChanged) {
            m_swrSamples = 0;
            if (!value.autoFallback && m_waitSwr) { m_waitSwr = false; m_state = TuningState::Matched; }
            return true; // Saving options does not create a new RF session.
        }
        m_session = m_pending = m_waitSwr = m_autoActive = m_override = false;
        m_needManual = value.managed;
        m_modeAttempted = m_autoAttempted = false;
        m_positive = 0; m_zeroSeen = m_powerBlocked = m_startHigh = false;
        m_state = value.managed ? TuningState::WaitingManual : TuningState::Native;
        return true;
    }
    void setPtt(bool known, bool transmitting) { m_pttKnown = known; m_pttTx = transmitting; }
    void observe(const tuner_meter_t& meter, uint32_t now) {
        const bool consecutive = m_hasMeter && now - m_meterAt <= FreshMs;
        m_hasMeter = true; m_meterAt = now; m_meter = meter; ++m_frames;
        if (!m_settings.managed) return;
        const bool valid = std::isfinite(meter.forward_power_watts) && meter.forward_power_watts >= 0;
        const bool rf = valid && meter.forward_power_raw > 0 && meter.forward_power_watts > 0;
        const bool zero = valid && meter.forward_power_raw == 0 && meter.forward_power_watts == 0;
        if (!consecutive) { m_zeroSeen = m_startHigh = false; m_swrSamples = 0; }
        if (m_waitSwr) {
            if (rf && meter.forward_power_watts < 100 && std::isfinite(meter.swr) &&
                meter.swr >= m_settings.swrTenths / 10.0) {
                if (m_swrSamples < 3) ++m_swrSamples;
            } else m_swrSamples = 0;
        }
        if (rf && (!m_pttKnown || m_pttTx)) {
            m_zeroSeen = false;
            if (!consecutive) m_positive = 0;
            if (!m_session && meter.forward_power_watts >= 100) m_startHigh = true;
            if (!m_session && ++m_positive >= 3) {
                m_session = true; m_pending = !m_startHigh; m_override = false;
                m_powerBlocked = m_startHigh;
                m_autoAttempted = m_modeAttempted = false; m_needManual = true;
                m_state = m_powerBlocked ? TuningState::HighPower : TuningState::WaitingAntenna;
            }
            if (m_session && meter.forward_power_watts >= 100 && !m_autoActive) {
                m_powerBlocked = true;
                m_pending = m_waitSwr = false; m_state = TuningState::HighPower;
            }
        } else {
            m_positive = 0; m_startHigh = false;
            if (zero && (!m_pttKnown || !m_pttTx)) {
                if (!m_zeroSeen) { m_zeroSeen = true; m_zeroAt = now; }
                if (m_session && (m_pttKnown || now - m_zeroAt >= RfOffMs)) {
                    m_session = m_pending = m_waitSwr = m_autoActive = m_override = false;
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
        if (m_waitSwr && allowTune) {
            if (m_frames != m_afterFrames && lowRf && std::isfinite(m_meter.swr) && m_meter.swr >= 1) {
                if (m_swrSamples >= 3 && !m_autoAttempted) {
                    m_waitSwr = false; m_autoAttempted = true;
                    return Action::Auto;
                }
                if (m_meter.swr < m_settings.swrTenths / 10.0) {
                    m_waitSwr = false; m_state = TuningState::Matched;
                }
            }
            if (m_waitSwr && now - m_tuneAt >= SwrWaitMs) {
                m_waitSwr = false; m_state = TuningState::NoFreshData;
            }
        }
        return Action::None;
    }
    void modeComplete(Action action, bool success) {
        if (!enabled()) return;
        if (action == Action::Manual) {
            if (success) {
                m_needManual = false;
                m_state = !m_session ? TuningState::WaitingRf :
                          m_powerBlocked ? TuningState::HighPower : TuningState::WaitingAntenna;
            }
            else { m_pending = m_waitSwr = false; m_state = TuningState::Error; }
        } else if (action == Action::Auto) {
            m_autoActive = success;
            m_state = success ? TuningState::AutoActive : TuningState::Error;
        }
    }
    void memoryComplete(bool success, uint32_t now) {
        m_tuneAt = now;
        m_afterFrames = m_frames;
        m_swrSamples = 0;
        m_waitSwr = m_session && !m_override && m_settings.autoFallback;
        m_state = m_waitSwr ? TuningState::WaitingSwr : success ? TuningState::Matched : TuningState::Error;
    }
    void antennaSelected() {
        if (enabled() && m_session && !m_override && !m_powerBlocked) {
            m_pending = true; m_waitSwr = m_autoActive = false; m_autoAttempted = false;
            m_state = TuningState::WaitingAntenna;
        }
    }
    void controlledSelected() {
        m_override = false; m_needManual = true; m_modeAttempted = true;
        m_autoActive = false; m_state = TuningState::WaitingManual;
    }
    void manualStarted() {
        m_pending = m_waitSwr = false;
        if (enabled()) { m_override = true; m_state = TuningState::ManualOverride; }
    }
    void invalidateFrame() { m_hasMeter = false; m_positive = 0; m_zeroSeen = m_startHigh = false; m_swrSamples = 0; }
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
    bool m_hasMeter = false, m_session = false, m_pending = false, m_waitSwr = false;
    bool m_needManual = false, m_modeAttempted = false, m_autoAttempted = false, m_autoActive = false;
    bool m_override = false, m_zeroSeen = false, m_pttKnown = false, m_pttTx = false;
    bool m_powerBlocked = false, m_startHigh = false;
    uint8_t m_positive = 0, m_swrSamples = 0;
    uint32_t m_meterAt = 0, m_zeroAt = 0, m_tuneAt = 0, m_frames = 0, m_afterFrames = 0;
};
}
