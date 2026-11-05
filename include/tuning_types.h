#pragma once
#include <stdint.h>
namespace ldg {
enum class TuningState : uint8_t {
    Native, WaitingManual, WaitingRf, WaitingAntenna, MemoryTune,
    WaitingSwr, Matched, AutoActive, ReturningManual, HighPower,
    NoFreshData, Error, ManualOverride
};
inline const char* tuningStateName(TuningState state) {
    switch (state) {
        case TuningState::WaitingManual: return "waiting_manual";
        case TuningState::WaitingRf: return "waiting_rf";
        case TuningState::WaitingAntenna: return "waiting_antenna";
        case TuningState::MemoryTune: return "memory_tune";
        case TuningState::WaitingSwr: return "waiting_swr";
        case TuningState::Matched: return "matched";
        case TuningState::AutoActive: return "auto_active";
        case TuningState::ReturningManual: return "returning_manual";
        case TuningState::HighPower: return "high_power";
        case TuningState::NoFreshData: return "no_fresh_data";
        case TuningState::Error: return "error";
        case TuningState::ManualOverride: return "manual_override";
        default: return "native";
    }
}
}
