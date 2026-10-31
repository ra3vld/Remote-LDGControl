#pragma once

#include "mqtt_protocol.h"
#include <cmath>

namespace ldg {

// HA displays the last observed value of each measurement between RF bursts.
// Keep this separate from the live web/LCD validity and never mix sources.
class MqttReadings {
public:
    MqttReadings() {
        m_meter.forward_power_watts = m_meter.reflected_power_watts = m_meter.swr = NAN;
    }

    void observe(const tuner_state_t& state) {
        if (!m_hasSource || m_remote != state.remote) {
            *this = MqttReadings{};
            m_hasSource = true;
            m_remote = state.remote;
        }
        const auto& meter = state.meter;
        m_forwardFresh = state.meterValid && validPower(meter.forward_power_watts);
        m_reflectedFresh = state.meterValid && validPower(meter.reflected_power_watts);
        m_swrFresh = state.meterValid && std::isfinite(meter.swr) && meter.swr >= 1.0;
        m_bandFresh = state.meterValid && meter.band > BAND_UNKNOWN && meter.band < BAND_COUNT;
        if (m_forwardFresh) {
            m_meter.forward_power_watts = meter.forward_power_watts;
            m_meter.forward_power_raw = meter.forward_power_raw;
        }
        if (m_reflectedFresh) {
            m_meter.reflected_power_watts = meter.reflected_power_watts;
            m_meter.reflected_power_raw = meter.reflected_power_raw;
        }
        if (m_swrFresh) m_meter.swr = meter.swr;
        if (m_bandFresh) m_meter.band = meter.band;
    }

    void write(JsonDocument& doc, const tuner_state_t& state) const {
        writeState(doc, state);
        // Non-finite unobserved readings remain null, rather than invented zeroes.
        doc["fwd_power"] = m_meter.forward_power_watts;
        doc["ref_power"] = m_meter.reflected_power_watts;
        doc["swr"] = m_meter.swr;
        doc["band"] = bandName(m_meter.band);
        doc["fwd_raw"] = nullptr;
        doc["ref_raw"] = nullptr;
        if (validPower(m_meter.forward_power_watts)) doc["fwd_raw"] = m_meter.forward_power_raw;
        if (validPower(m_meter.reflected_power_watts)) doc["ref_raw"] = m_meter.reflected_power_raw;
        doc["telemetry_fresh"] = state.meterValid;
        JsonObject held = doc["held"].to<JsonObject>();
        held["fwd_power"] = validPower(m_meter.forward_power_watts) && !m_forwardFresh;
        held["ref_power"] = validPower(m_meter.reflected_power_watts) && !m_reflectedFresh;
        held["swr"] = std::isfinite(m_meter.swr) && !m_swrFresh;
        held["band"] = m_meter.band != BAND_UNKNOWN && !m_bandFresh;
    }

private:
    static bool validPower(double value) { return std::isfinite(value) && value >= 0.0; }
    tuner_meter_t m_meter{};
    bool m_hasSource = false, m_remote = false;
    bool m_forwardFresh = false, m_reflectedFresh = false, m_swrFresh = false, m_bandFresh = false;
};

} // namespace ldg
