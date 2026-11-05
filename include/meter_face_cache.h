#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

namespace ldg {
// Lossless sparse RGB565 scanlines. A tag encodes 1..128 background or
// literal pixels. Row offsets allow restoring only the current dirty area.
// Allocate the measured compressed size, never a full-screen framebuffer.
class MeterFaceCache {
public:
    static constexpr unsigned MaxBytes = 48 * 1024;
    static constexpr int MaxWidth = 480, MaxHeight = 320;
    ~MeterFaceCache() { clear(); }
    MeterFaceCache() = default;
    MeterFaceCache(const MeterFaceCache&) = delete;
    MeterFaceCache& operator=(const MeterFaceCache&) = delete;
    void clear() {
        std::free(m_data); m_data = nullptr; m_valid = false;
        m_used = m_capacity = 0; m_rows = 0;
    }
    bool count(int width, int height, uint16_t background) {
        clear(); m_width = width; m_height = height; m_background = background;
        return width > 0 && width <= MaxWidth && height > 0 && height <= MaxHeight;
    }
    bool append(const uint16_t* pixels) {
        if (m_rows >= m_height) return false;
        m_offsets[m_rows++] = uint16_t(m_used);
        for (int x = 0; x < m_width;) {
            const bool literal = pixels[x] != m_background;
            int length = 1;
            while (x + length < m_width && length < 128 &&
                   (pixels[x + length] != m_background) == literal) ++length;
            const unsigned size = 1 + (literal ? 2 * length : 0);
            if (m_used + size > MaxBytes || (m_data && m_used + size > m_capacity)) return false;
            if (m_data) {
                m_data[m_used] = uint8_t((literal ? 128 : 0) | (length - 1));
                if (literal) std::memcpy(m_data + m_used + 1, pixels + x, 2 * length);
            }
            m_used += size; x += length;
        }
        m_offsets[m_rows] = uint16_t(m_used);
        return true;
    }
    bool allocate() {
        if (m_rows != m_height || !m_used) return false;
        m_capacity = m_used;
        m_data = static_cast<uint8_t*>(std::malloc(m_capacity));
        m_used = 0; m_rows = 0;
        return m_data != nullptr;
    }
    bool finish() { return m_valid = m_data && m_rows == m_height && m_used == m_capacity; }
    bool valid() const { return m_valid; }
    unsigned bytes() const { return m_valid ? m_capacity : 0; }
    bool copyRow(uint16_t* destination, int y, int from, int to) const {
        if (!m_valid || y < 0 || y >= m_height || from < 0 || to >= m_width || from > to) return false;
        std::fill(destination, destination + to - from + 1, m_background);
        unsigned offset = m_offsets[y];
        for (int x = 0; x <= to && offset < m_offsets[y + 1];) {
            const uint8_t tag = m_data[offset++];
            const int length = (tag & 127) + 1;
            if (tag & 128) {
                const int left = std::max(x, from), right = std::min(x + length - 1, to);
                if (left <= right) std::memcpy(destination + left - from,
                    m_data + offset + 2 * (left - x), 2 * (right - left + 1));
                offset += 2 * length;
            }
            x += length;
        }
        return true;
    }
private:
    uint8_t* m_data = nullptr;
    uint16_t m_offsets[MaxHeight + 1] = {};
    unsigned m_used = 0, m_capacity = 0;
    int m_width = 0, m_height = 0, m_rows = 0;
    uint16_t m_background = 0;
    bool m_valid = false;
};
}
