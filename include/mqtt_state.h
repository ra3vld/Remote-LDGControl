#ifndef MQTT_STATE_H
#define MQTT_STATE_H

#include "tuner_types.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdint.h>

namespace ldg {

inline bool numericChanged(double previous, double current, double absolute,
                           double relative = 0.0, bool zeroTransition = false) {
    if (std::isfinite(previous) != std::isfinite(current)) return true;
    if (!std::isfinite(current)) return false;
    if (zeroTransition && ((previous == 0.0) != (current == 0.0))) return true;
    const double threshold = std::max(absolute, std::abs(previous) * relative);
    const double tolerance = 8 * std::numeric_limits<double>::epsilon() *
        std::max(threshold, std::max(std::abs(previous), std::abs(current)));
    return std::abs(current - previous) >= threshold - tolerance;
}

inline bool significantChange(const tuner_state_t& previous, const tuner_state_t& current) {
    if (previous.remote != current.remote || previous.mode != current.mode ||
        previous.antenna != current.antenna || previous.meterValid != current.meterValid ||
        lastKnownBand(previous) != lastKnownBand(current) || bandIsHeld(previous) != bandIsHeld(current))
        return true;
    if (!current.meterValid) return false;
    return previous.meter.band != current.meter.band ||
        numericChanged(previous.meter.forward_power_watts, current.meter.forward_power_watts,
                       1.0, 0.01, true) ||
        numericChanged(previous.meter.reflected_power_watts, current.meter.reflected_power_watts,
                       0.1, 0.01, true) ||
        numericChanged(previous.meter.swr, current.meter.swr, 0.05);
}

// Only a successful transport write commits the baseline and advances the clock.
// Latch changes until the next send so a short event is not lost to throttling.
class MqttStateSchedule {
public:
    static constexpr uint32_t ACTIVE_MS = 1000;
    static constexpr uint32_t IDLE_MS = 30000;

    void requestImmediate() { m_immediate = true; }
    bool due(uint32_t now, const tuner_state_t& state) {
        if (m_hasPublished && significantChange(m_previous, state)) m_pending = true;
        const uint32_t elapsed = now - m_lastPublished;
        return m_immediate || !m_hasPublished || elapsed >= IDLE_MS ||
               (m_pending && elapsed >= ACTIVE_MS);
    }
    void published(uint32_t now, const tuner_state_t& state) {
        m_previous = state;
        m_lastPublished = now;
        m_hasPublished = true;
        m_pending = false;
        m_immediate = false;
    }

private:
    tuner_state_t m_previous;
    uint32_t m_lastPublished = 0;
    bool m_hasPublished = false;
    bool m_pending = false;
    bool m_immediate = false;
};

// Caller protects this value store when accessed from multiple FreeRTOS tasks.
class ActiveTunerState {
public:
    void updateRemote(const tuner_state_t& state) {
        const tuner_band_t previousBand = lastKnownBand(m_remote);
        m_remote = state;
        m_remote.lastBand = knownBand(lastKnownBand(state)) ? lastKnownBand(state) : previousBand;
        m_remote.remote = true;
        m_connected = true;
    }
    void disconnectRemote() {
        m_connected = false;
        m_remote = tuner_state_t{};
    }
    bool remoteConnected() const { return m_connected; }
    bool remoteConnected(uint32_t telemetryAge, bool commandActive) const {
        return m_connected && (commandActive || telemetryAge <= 3000);
    }
    tuner_state_t select(const tuner_state_t& local) const {
        return m_connected ? m_remote : local;
    }
    tuner_state_t select(const tuner_state_t& local, uint32_t telemetryAge, bool commandActive) const {
        if (!remoteConnected(telemetryAge, commandActive)) return local;
        tuner_state_t state = m_remote;
        if (telemetryAge > 3000) state.meterValid = false;
        return state;
    }

private:
    tuner_state_t m_remote;
    bool m_connected = false;
};

} // namespace ldg
#endif // MQTT_STATE_H
