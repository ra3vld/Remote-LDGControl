#pragma once

#include "antenna_settings.h"
#include <cmath>

namespace ldg {
// UART-worker owned. Only actual decoded frames enter observe(); HTTP polls,
// held band values and MQTT retained readings cannot produce transitions.
class AntennaAutomation {
public:
    static constexpr uint32_t FreshMs = 500;
    enum class Action { None, Probe, Select };
    struct Request {
        Action action;
        tuner_ant_t antenna;
        Request(Action a = Action::None, tuner_ant_t ant = ANT_UNKNOWN) : action(a), antenna(ant) {}
    };

    bool configure(const AntennaSettings& settings) {
        if (sameAntennaSettings(m_settings, settings)) return false;
        m_settings = settings;
        m_pending = false;
        resetCandidate();
        m_auto = settings.enabled ? AntennaAuto::Idle : AntennaAuto::Disabled;
        return true;
    }
    void invalidateFrame() {
        m_hasMeter = false;
        resetCandidate();
    }
    void observe(const tuner_meter_t& meter, uint32_t now, uint32_t manualRevision) {
        if (!m_hasMeter || now - m_meterAt > FreshMs) resetCandidate();
        m_hasMeter = true;
        m_meterAt = now;
        m_power = meter.forward_power_watts;
        m_zero = meter.forward_power_raw == 0 && validPower(m_power) && m_power == 0;
        const bool rf = meter.forward_power_raw > 0 || (validPower(m_power) && m_power > 0);
        if (!rf || !knownBand(meter.band)) { resetCandidate(); return; }
        if (meter.band == m_currentBand) { resetCandidate(); return; }
        if (meter.band != m_candidate) {
            resetCandidate();
            m_candidate = meter.band;
        }
        m_badPower |= !validPower(m_power);
        m_highPower |= validPower(m_power) && m_power >= 100;
        if (++m_count < 3) return;
        const bool first = !knownBand(m_currentBand);
        m_currentBand = m_candidate; // Consume the transition even when skipped.
        m_pending = false;
        if (!m_settings.enabled) m_auto = AntennaAuto::Disabled;
        else if (first) m_auto = AntennaAuto::Idle;
        else if (m_currentBand == m_manualBand) m_auto = AntennaAuto::Manual;
        else if (m_init != AntennaInit::Ready) m_auto = AntennaAuto::Uninitialized;
        else if (m_badPower) m_auto = AntennaAuto::InvalidPower;
        else if (m_highPower) m_auto = AntennaAuto::HighPower;
        else if (m_settings.bands[m_currentBand] == ANT_UNKNOWN) m_auto = AntennaAuto::Unassigned;
        else {
            m_pending = true;
            m_target = static_cast<tuner_ant_t>(m_settings.bands[m_currentBand]);
            m_pendingRevision = manualRevision;
            m_auto = AntennaAuto::Pending;
        }
        m_manualBand = BAND_UNKNOWN;
        resetCandidate();
    }
    Request poll(uint32_t now, uint32_t manualRevision, bool manualPending, tuner_ant_t selected) {
        const bool fresh = m_hasMeter && now - m_meterAt <= FreshMs;
        if (m_init == AntennaInit::Waiting || m_init == AntennaInit::WaitingRfOff) {
            m_init = fresh && !m_zero ? AntennaInit::WaitingRfOff : AntennaInit::Waiting;
            if (!manualPending && fresh && m_zero) {
                m_init = AntennaInit::Checking;
                return Request(Action::Probe);
            }
        }
        if (!m_pending) return {};
        m_pending = false; // A decision gets exactly one execution opportunity.
        if (manualPending || manualRevision != m_pendingRevision) m_auto = AntennaAuto::Manual;
        else if (!fresh || !validPower(m_power)) m_auto = AntennaAuto::InvalidPower;
        else if (m_power >= 100) m_auto = AntennaAuto::HighPower;
        else if (selected == m_target) m_auto = AntennaAuto::AlreadySelected;
        else return Request(Action::Select, m_target);
        return {};
    }
    void restoring() { m_init = AntennaInit::Restoring; }
    void probeComplete(bool success) { m_init = success ? AntennaInit::Ready : AntennaInit::Error; }
    void selectionComplete(bool success) { m_auto = success ? AntennaAuto::Selected : AntennaAuto::Error; }
    void manualStarted() {
        if (m_pending) m_auto = AntennaAuto::Manual;
        m_pending = false;
        // A transition whose confirmation straddles a manual command must not
        // subsequently undo that command on its third frame.
        m_manualBand = knownBand(m_candidate) ? m_candidate : m_currentBand;
    }
    void manualAntennaComplete(bool success) {
        if (success) m_init = AntennaInit::Ready;
    }
    AntennaInit init() const { return m_init; }
    AntennaAuto automatic() const { return m_auto; }
    tuner_band_t currentBand() const { return m_currentBand; }

private:
    static bool validPower(double power) { return std::isfinite(power) && power >= 0; }
    void resetCandidate() {
        m_candidate = BAND_UNKNOWN; m_count = 0;
        m_badPower = m_highPower = false;
    }
    AntennaSettings m_settings{};
    AntennaInit m_init = AntennaInit::Waiting;
    AntennaAuto m_auto = AntennaAuto::Disabled;
    tuner_band_t m_currentBand = BAND_UNKNOWN, m_candidate = BAND_UNKNOWN, m_manualBand = BAND_UNKNOWN;
    tuner_ant_t m_target = ANT_UNKNOWN;
    unsigned m_count = 0;
    bool m_badPower = false, m_highPower = false, m_pending = false;
    bool m_hasMeter = false, m_zero = false;
    double m_power = 0;
    uint32_t m_meterAt = 0, m_pendingRevision = 0;
};
}
