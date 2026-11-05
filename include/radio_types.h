#pragma once

#include <stdint.h>

namespace ldg {
enum class RadioProvider : uint8_t { None, Tci, OmniRig };
inline const char* radioProviderName(RadioProvider p) { return p==RadioProvider::Tci ? "tci" : p==RadioProvider::OmniRig ? "omnirig" : "none"; }
enum class RadioConnection : uint8_t { Disabled, Disconnected, Connecting, Synchronizing, Ready };
inline const char* radioConnectionName(RadioConnection value) {
    switch (value) {
        case RadioConnection::Disconnected: return "disconnected";
        case RadioConnection::Connecting: return "connecting";
        case RadioConnection::Synchronizing: return "synchronizing";
        case RadioConnection::Ready: return "ready";
        default: return "disabled";
    }
}

// Value snapshot: no borrowed parser buffers or network objects cross tasks.
struct RadioState {
    static constexpr uint32_t LivenessMs = 3000;
    RadioProvider provider = RadioProvider::None;
    RadioConnection connection = RadioConnection::Disabled;
    uint8_t receiver = 0, receiverCount = 0;
    bool frequencyValid = false;
    bool rxKnown = false, transmitting = false, tuneKnown = false, tuning = false;
    bool splitKnown = false, split = false, xitKnown = false, xit = false;
    int32_t xitOffset = 0;
    uint64_t rxFrequencyHz = 0, txFrequencyHz = 0;
    uint32_t revision = 0, ageMs = 0, stableMs = 0;
    uint32_t freeHeap = 0, largestBlock = 0, stackFree = 0;
    char device[32]{}, protocol[24]{}, modulation[16]{}, error[64]{};

    bool ownsBand() const {
        return connection == RadioConnection::Synchronizing || connection == RadioConnection::Ready;
    }
    bool safeRx() const {
        return connection == RadioConnection::Ready && frequencyValid && rxKnown && !transmitting &&
               tuneKnown && !tuning && ageMs <= LivenessMs;
    }
};
}
