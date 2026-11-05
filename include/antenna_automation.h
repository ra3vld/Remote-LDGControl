#pragma once

#include "antenna_settings.h"
#include <cmath>

namespace ldg {
// UART-worker owned. Only actual decoded frames enter observe(); HTTP polls,
// held band values and MQTT retained readings cannot produce transitions.
class AntennaAutomation {
public:
    static constexpr uint32_t FreshMs = 500;
    static constexpr uint32_t MeterWaitMs = 1500;
    // The specified full tune takes up to 15 s. Allow its failure path to
    // finish before one deferred in-RF recovery, without requiring a match.
    static constexpr uint32_t TunerWaitMs = 20000;
    enum class Action { None, Probe, Select, RefreshMeter };
    struct Request {
        Action action;
        tuner_ant_t antenna;
        uint8_t recovery;
        Request(Action a = Action::None, tuner_ant_t ant = ANT_UNKNOWN, uint8_t r = 0) : action(a), antenna(ant), recovery(r) {}
    };

    bool configure(const AntennaSettings& settings) {
        if (sameAntennaSettings(m_settings, settings)) return false;
        m_settings = settings;
        m_pending = false;
        m_busyRecoveries = 0;
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
        m_meterBand = meter.band;
        m_zero = meter.forward_power_raw == 0 && validPower(m_power) && m_power == 0;
        if (m_pending && validPower(m_power) && m_power >= 100) {
            m_pending = false;
            m_busyRecoveries = 0;
            m_auto = AntennaAuto::HighPower;
        }
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
        const bool firstManual = !knownBand(m_currentBand) && m_manualBeforeBand;
        m_currentBand = m_candidate; // Consume the transition even when skipped.
        m_pending = false;
        m_busyRecoveries = 0;
        if (!m_settings.enabled) m_auto = AntennaAuto::Disabled;
        else if (firstManual || m_currentBand == m_manualBand) m_auto = AntennaAuto::Manual;
        else if (m_badPower) m_auto = AntennaAuto::InvalidPower;
        else if (m_highPower) m_auto = AntennaAuto::HighPower;
        else if (m_settings.bands[m_currentBand] == ANT_UNKNOWN) m_auto = AntennaAuto::Unassigned;
        else {
            m_pending = true;
            m_target = static_cast<tuner_ant_t>(m_settings.bands[m_currentBand]);
            m_pendingRevision = manualRevision;
            m_meterRefreshRequested = false;
            m_auto = AntennaAuto::Pending;
        }
        m_manualBeforeBand = false;
        m_manualBand = BAND_UNKNOWN;
        resetCandidate();
    }
    Request poll(uint32_t now, uint32_t manualRevision, bool manualPending, tuner_ant_t selected,
                 bool waitForRfOff = false) {
        const bool fresh = m_hasMeter && now - m_meterAt <= FreshMs;
        if (m_init == AntennaInit::Waiting || m_init == AntennaInit::WaitingRfOff) {
            m_init = fresh && !m_zero ? AntennaInit::WaitingRfOff : AntennaInit::Waiting;
            if (!manualPending && fresh && m_zero) {
                m_init = AntennaInit::Checking;
                return Request(Action::Probe);
            }
        }
        if (!m_pending) return {};
        if (manualPending || manualRevision != m_pendingRevision) m_auto = AntennaAuto::Manual;
        else if (m_init != AntennaInit::Ready) {
            m_auto = AntennaAuto::Uninitialized;
            return {}; // Keep the first band choice through the startup check.
        }
        else if (m_busyRecoveries) {
            m_auto = AntennaAuto::WaitingTuner;
            // A busy tuner can stop telemetry while searching. Missing data is
            // neither zero power nor a reason to forget the requested port.
            if (!fresh || !validPower(m_power)) return {};
            if (m_power >= 100) m_auto = AntennaAuto::HighPower;
            else if (!m_zero && (m_meterBand != m_currentBand || m_busyRecoveries > 1 ||
                                 now - m_busyAt < TunerWaitMs)) return {};
            else if (selected == m_target) m_auto = AntennaAuto::AlreadySelected;
            else {
                m_pending = false;
                return Request(Action::Select, m_target, m_busyRecoveries);
            }
        }
        else if (!fresh || !validPower(m_power)) {
            if (!m_meterRefreshRequested) {
                m_meterRefreshRequested = true;
                m_meterRefreshAt = now;
                m_auto = AntennaAuto::WaitingMeter;
                return Request(Action::RefreshMeter);
            }
            if (now - m_meterRefreshAt < MeterWaitMs) {
                m_auto = AntennaAuto::WaitingMeter;
                return {};
            }
            m_auto = AntennaAuto::InvalidPower;
        }
        else if (m_power >= 100) m_auto = AntennaAuto::HighPower;
        else if (selected == m_target) m_auto = AntennaAuto::AlreadySelected;
        else if (waitForRfOff && !m_zero) {
            // The protocol cannot query AUTO/MANUAL. Preserve an unknown mode
            // and wait for actual zero-power telemetry, without sending A yet.
            m_auto = AntennaAuto::WaitingRfOff;
            return {};
        } else {
            m_pending = false;
            return Request(Action::Select, m_target);
        }
        m_pending = false; // A decision gets exactly one execution opportunity.
        m_busyRecoveries = 0;
        return {};
    }
    void restoring() { m_init = AntennaInit::Restoring; }
    void probeComplete(bool success) { m_init = success ? AntennaInit::Ready : AntennaInit::Error; }
    void selectionComplete(bool success) { m_busyRecoveries = 0; m_auto = success ? AntennaAuto::Selected : AntennaAuto::Error; }
    bool selectionDeferred(tuner_band_t band, uint32_t now) {
        if (band != m_currentBand || m_busyRecoveries >= 2) return false;
        ++m_busyRecoveries;
        m_busyAt = now;
        m_pending = true;
        m_auto = AntennaAuto::WaitingTuner;
        return true;
    }
    bool waitingTuner() const { return m_pending && m_busyRecoveries != 0; }
    void selectionTuneStatus(AntennaAuto status) { m_auto = status; }
    void selectionPowerChanged(bool valid) { m_busyRecoveries = 0; m_auto = valid ? AntennaAuto::HighPower : AntennaAuto::InvalidPower; }
    void selectionRetrying() { m_auto = AntennaAuto::Retrying; }
    void meterRefreshFailed() { m_pending = false; m_auto = AntennaAuto::InvalidPower; }
    void manualStarted() {
        if (m_pending) m_auto = AntennaAuto::Manual;
        m_pending = false;
        m_busyRecoveries = 0;
        // A transition whose confirmation straddles a manual command must not
        // subsequently undo that command on its third frame.
        m_manualBand = knownBand(m_candidate) ? m_candidate : m_currentBand;
        if (knownBand(m_manualBand)) m_manualBeforeBand = false;
    }
    void manualAntennaComplete(bool success) {
        if (success) {
            m_init = AntennaInit::Ready;
            if (!knownBand(m_currentBand) && !knownBand(m_candidate)) m_manualBeforeBand = true;
        }
        if (m_settings.enabled) m_auto = success ? AntennaAuto::ManualSelection : AntennaAuto::Manual;
    }
    AntennaInit init() const { return m_init; }
    bool enabled() const { return m_settings.enabled; }
    AntennaAuto automatic() const { return m_auto; }
    tuner_band_t currentBand() const { return m_currentBand; }
    bool hasFreshMeter(uint32_t now) const { return m_hasMeter && now - m_meterAt <= FreshMs; }

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
    tuner_band_t m_meterBand = BAND_UNKNOWN;
    tuner_ant_t m_target = ANT_UNKNOWN;
    unsigned m_count = 0;
    bool m_badPower = false, m_highPower = false, m_pending = false;
    bool m_manualBeforeBand = false;
    bool m_hasMeter = false, m_zero = false;
    bool m_meterRefreshRequested = false;
    double m_power = 0;
    uint32_t m_meterAt = 0, m_pendingRevision = 0, m_meterRefreshAt = 0;
    uint32_t m_busyAt = 0;
    uint8_t m_busyRecoveries = 0;
};
}
