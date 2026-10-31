#pragma once
#include <atomic>
#include <stdint.h>
#include "tuner_types.h"

namespace ldg {
// One atomic value keeps the command, its generation and its outcome together.
// A queued older operation must never finish the newer operation's indicator.
class CommandFeedback {
public:
    enum class Phase : uint8_t { Idle, Pending, Success, Failed };
    struct Snapshot {
        uint32_t token;
        uint8_t command;
        Phase phase;
    };
    uint32_t start(uint8_t command) {
        uint32_t previous = m_value.load(), next;
        do { next = ((previous + 1024u) & ~1023u) | (uint32_t(Phase::Pending) << 8) | command; }
        while (!m_value.compare_exchange_weak(previous, next));
        return next;
    }
    void finish(uint32_t token, bool success) {
        const uint32_t next = (token & ~(3u << 8)) | (uint32_t(success ? Phase::Success : Phase::Failed) << 8);
        m_value.compare_exchange_strong(token, next);
    }
    Snapshot snapshot() const {
        const uint32_t value = m_value.load();
        return {value, uint8_t(value), Phase((value >> 8) & 3)};
    }
private:
    std::atomic<uint32_t> m_value{0};
};

inline const char* commandLabel(uint8_t command) {
    switch (command) {
        case TUNER_CMD_MEM_TUNE: return "MEM TUNE";
        case TUNER_CMD_FULL_TUNE: return "FULL TUNE";
        case TUNER_CMD_SELECT_ANT_A: return "ANT 1";
        case TUNER_CMD_SELECT_ANT_B: return "ANT 2";
        case TUNER_CMD_TOGGLE_ANT: return "ANT";
        case TUNER_CMD_BYPASS: return "BYPASS";
        case TUNER_CMD_AUTO_MODE: return "AUTO";
        case TUNER_CMD_MANUAL_MODE: return "MANUAL";
        default: return "COMMAND";
    }
}
}
