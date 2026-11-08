#pragma once
#include <stdint.h>
#include <string>
#include <utility>

namespace ldg {
// One telemetry frame per subscriber. A newer snapshot can replace a frame
// only before its first byte is sent; a partially sent frame must finish.
class WebsocketOutput {
public:
    static constexpr size_t MaxFrame = 1540;
    static constexpr uint32_t NoProgressMs = 10000;
    bool submit(std::string frame, uint32_t now) {
        if (frame.size() > MaxFrame) return false;
        if (pending()) {
            ++m_skipped;
            if (m_offset) return true;
        } else {
            m_lastProgress = now;
        }
        m_frame.swap(frame);
        return true;
    }
    template<class Writer> bool flush(uint32_t now, Writer writer) {
        if (!pending()) return true;
        if (uint32_t(now - m_lastProgress) >= NoProgressMs) return false;
        const size_t remaining = m_frame.size() - m_offset;
        const int count = writer(m_frame.data() + m_offset, remaining);
        if (count < 0 || size_t(count) > remaining) return false;
        if (count) {
            m_offset += size_t(count);
            m_lastProgress = now;
            if (m_offset == m_frame.size()) {
                m_frame.clear();
                m_offset = 0;
            }
        }
        return true;
    }
    bool pending() const { return !m_frame.empty(); }
    uint32_t skipped() const { return m_skipped; }
private:
    std::string m_frame;
    size_t m_offset = 0;
    uint32_t m_lastProgress = 0, m_skipped = 0;
};
}
