#pragma once
#include <math.h>

namespace ldg {
// Two decades of extra low-power resolution, continuous at zero. Apply the
// same mapping to needles, tick values and both coordinates of the SWR locus.
inline float meterFraction(float power, float maximum, bool logarithmic) {
    const float fraction = isfinite(power) && power > 0 && maximum > 0
        ? fminf(power / maximum, 1.0f) : 0.0f;
    return logarithmic ? log1pf(99.0f * fraction) / logf(100.0f) : fraction;
}
inline float meterPower(float fraction, float maximum, bool logarithmic) {
    return maximum * (logarithmic ? expm1f(fraction * logf(100.0f)) / 99.0f : fraction);
}
}
