#pragma once
#include <math.h>
#include <stdint.h>

namespace ldg {
// Time-based angular smoothing. The target is always the latest RF sample;
// no sample queue, overshoot or smoothing of measurement/control telemetry.
class NeedleMotion {
public:
    void target(float value) { m_target = clamp(value); }
    void snap(float value, uint32_t now) { m_current = m_target = clamp(value); m_at = now; m_started = true; }
    float advance(uint32_t now) {
        const uint32_t elapsed = m_started ? now - m_at : 30;
        m_started = true; m_at = now;
        if (elapsed > 250) m_current = m_target; // Resume at current data, never replay a stall.
        else {
            const float tau = m_target > m_current ? 40.0f : 65.0f;
            m_current += (m_target - m_current) * -expm1f(-float(elapsed) / tau);
            if (fabsf(m_target - m_current) < 0.0008f) m_current = m_target;
        }
        return m_current;
    }
    float value() const { return m_current; }
private:
    static float clamp(float value) { return isfinite(value) ? fminf(fmaxf(value, 0), 1) : 0; }
    float m_current = 0, m_target = 0;
    uint32_t m_at = 0;
    bool m_started = false;
};
}
