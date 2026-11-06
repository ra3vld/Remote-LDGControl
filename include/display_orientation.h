#pragma once
#include <stdint.h>

namespace ldg {
// Relative to the established 480x320 landscape: two quarter-turn bits,
// followed by horizontal/vertical reflection in the resulting screen axes.
// No instance, lookup table, pixel buffer or additional driver state.
inline uint8_t displayDriverRotation(uint8_t mode) {
    return ((1 + (mode & 3)) & 3) | (mode & 12);
}
inline uint8_t displayMadctl(uint8_t driverRotation) {
    uint8_t value = 0;
    switch (driverRotation & 3) {
        case 0: value = 0x40; break; // MX
        case 1: value = 0x20; break; // MV
        case 2: value = 0x80; break; // MY
        case 3: value = 0xe0; break; // MX | MY | MV
    }
    if (driverRotation & 4) value ^= (value & 0x20) ? 0x80 : 0x40;
    if (driverRotation & 8) value ^= (value & 0x20) ? 0x40 : 0x80;
    return value;
}
// Inverse of the hardware mapping: calibrated landscape touch -> displayed UI.
inline void orientDisplayTouch(int16_t& x, int16_t& y, uint8_t driverRotation) {
    const int16_t beforeX = x, beforeY = y;
    switch ((driverRotation + 3) & 3) {
        case 1: x = beforeY; y = 479 - beforeX; break;
        case 2: x = 479 - beforeX; y = 319 - beforeY; break;
        case 3: x = 319 - beforeY; y = beforeX; break;
    }
    const bool portrait = !(driverRotation & 1);
    if (driverRotation & 4) x = (portrait ? 319 : 479) - x;
    if (driverRotation & 8) y = (portrait ? 479 : 319) - y;
}
}
