#pragma once

#include <math.h>
#include <stdint.h>

namespace ldg {

// Geometry does not depend on telemetry. Cache it once per widget size rather
// than rebuilding every scale and SWR curve for each LVGL draw-buffer strip.
class MeterGeometry {
public:
    struct Point { int16_t x, y; };
    struct Line { Point from, to; };
    struct Tick { Line line; Point label; };
    struct Curve { Line lines[39]; uint8_t count = 0; };

    int centerX = 0, centerY = 0, radius = 0;
    Point forwardPivot{}, reflectedPivot{};
    Tick ticks[2][9]{};
    Curve curves[5]{};

    bool resize(int width, int height) {
        if (width == m_width && height == m_height) return false;
        m_width = width;
        m_height = height;
        centerX = width / 2;
        centerY = height - 20;
        radius = centerY - 20;
        const int halfBase = width / 2 - 20;
        forwardPivot = {int16_t(centerX + halfBase), int16_t(centerY)};
        reflectedPivot = {int16_t(centerX - halfBase), int16_t(centerY)};
        buildTicks(0, forwardPivot, pi(), pi() / 2.0f);
        buildTicks(1, reflectedPivot, 0.0f, pi() / 2.0f);

        static const float values[] = {1.5f, 2.0f, 3.0f, 5.0f, 10.0f};
        const float base = float(forwardPivot.x - reflectedPivot.x);
        for (int si = 0; si < 5; ++si) {
            Curve& curve = curves[si];
            curve.count = 0;
            const float g = (values[si] - 1.0f) / (values[si] + 1.0f);
            const float g2 = g * g;
            bool havePrevious = false;
            Point previous{};
            for (int k = 1; k <= 40; ++k) {
                const float fraction = float(k) / 40.0f;
                const float reflected = fminf(g2 * fraction, 1.0f);
                const float forwardAngle = pi() - fraction * (pi() / 2.0f);
                const float reflectedAngle = reflected * (pi() / 2.0f);
                const float sinR = sinf(reflectedAngle);
                const float sinD = sinf(forwardAngle - reflectedAngle);
                if (fabsf(sinD) < 0.001f || fabsf(sinR) < 0.001f) {
                    havePrevious = false;
                    continue;
                }
                const float t = base * sinR / (float(radius) * sinD);
                if (t < 0.0f || t > 1.05f) { havePrevious = false; continue; }
                const Point point = {
                    int16_t(forwardPivot.x + int(t * float(radius) * cosf(forwardAngle))),
                    int16_t(forwardPivot.y - int(t * float(radius) * sinf(forwardAngle)))
                };
                if (havePrevious) curve.lines[curve.count++] = {previous, point};
                previous = point;
                havePrevious = true;
            }
        }
        m_forwardFraction = m_reflectedFraction = -1.0f;
        return true;
    }

    Point forwardNeedle(float fraction) {
        if (fraction != m_forwardFraction) {
            m_forwardFraction = fraction;
            const float angle = pi() - fraction * (pi() / 2.0f);
            m_forward = {int16_t(forwardPivot.x + int(radius * cosf(angle))),
                         int16_t(forwardPivot.y - int(radius * sinf(angle)))};
        }
        return m_forward;
    }

    Point reflectedNeedle(float fraction) {
        if (fraction != m_reflectedFraction) {
            m_reflectedFraction = fraction;
            const float angle = fraction * (pi() / 2.0f);
            m_reflected = {int16_t(reflectedPivot.x + int(radius * cosf(angle))),
                           int16_t(reflectedPivot.y - int(radius * sinf(angle)))};
        }
        return m_reflected;
    }

private:
    int m_width = 0, m_height = 0;
    float m_forwardFraction = -1, m_reflectedFraction = -1;
    Point m_forward{}, m_reflected{};
    static float pi() { return 3.14159265f; }

    void buildTicks(int side, Point pivot, float zero, float full) {
        for (int i = 0; i <= 8; ++i) {
            const float fraction = float(i) / 8.0f;
            const float angle = zero + fraction * (full - zero);
            const float c = cosf(angle), s = sinf(angle);
            const int length = i % 2 == 0 ? 10 : 5;
            const int labelRadius = radius - length - 12;
            ticks[side][i] = {
                {{int16_t(pivot.x + int(c * radius)), int16_t(pivot.y - int(s * radius))},
                 {int16_t(pivot.x + int(c * (radius - length))), int16_t(pivot.y - int(s * (radius - length)))}},
                {int16_t(pivot.x + int(c * labelRadius)), int16_t(pivot.y - int(s * labelRadius))}
            };
        }
    }
};

} // namespace ldg
