#pragma once
#include "tuner_types.h"

namespace ldg {
// Broad amateur-band envelopes, independent of radio model and local TX permissions.
// This is an antenna table, not a regulatory permission to transmit.
inline tuner_band_t frequencyBand(uint64_t hz) {
    struct Range { uint64_t low, high; tuner_band_t band; };
    static const Range ranges[] = {
        {1800000, 2000000, BAND_160M}, {3500000, 4000000, BAND_80M},
        {5000000, 5500000, BAND_60M}, {7000000, 7300000, BAND_40M},
        {10100000, 10150000, BAND_30M}, {14000000, 14350000, BAND_20M},
        {18068000, 18168000, BAND_17M}, {21000000, 21450000, BAND_15M},
        {24890000, 24990000, BAND_12M}, {28000000, 29700000, BAND_10M},
        {50000000, 54000000, BAND_6M}
    };
    for (const auto& range : ranges) if (hz >= range.low && hz <= range.high) return range.band;
    return BAND_UNKNOWN;
}
inline const char* radioBandName(uint64_t hz) {
    static const char* const names[] = {"unknown", "160m", "80m", "60m", "40m", "30m", "20m",
        "17m", "15m", "12m", "10m", "6m"};
    return names[frequencyBand(hz)];
}
}
