#pragma once

#include <stddef.h>
#include <stdint.h>

// GT911 register protocol. The bus is injected so host tests exercise the
// same transactions and coordinate mapping used on the board.
template<class Bus>
class Gt911Touch {
public:
    struct Point {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = false;
    };

    explicit Gt911Touch(Bus& bus) : m_bus(bus) {}

    bool begin() {
        m_point = Point{};
        const uint8_t addresses[] = {0x5d, 0x14};
        for (uint8_t address : addresses) {
            m_address = address;
            uint8_t product[4];
            if (read(0x8140, product, sizeof(product)) &&
                product[0] == '9' && product[1] == '1' && product[2] == '1') {
                return true;
            }
        }
        m_address = 0;
        return false;
    }

    uint8_t address() const { return m_address; }

    Point poll(uint32_t now) {
        if (!m_address) {
            if (uint32_t(now - m_lastProbe) < 1000) return Point{};
            m_lastProbe = now;
            if (!begin()) return Point{};
        }

        uint8_t status;
        if (!read(0x814e, &status, 1)) return release();
        if (!(status & 0x80)) {
            // No new report is not a release, but never keep a stale press.
            if (uint32_t(now - m_lastReport) > 100) return release();
            return m_point;
        }

        Point point = m_point;
        point.pressed = false;
        const uint8_t count = status & 0x0f;
        if (count == 1) {
            uint8_t record[8];
            if (!read(0x814f, record, sizeof(record))) return release();
            const uint16_t x = record[1] | (uint16_t(record[2]) << 8);
            const uint16_t y = record[3] | (uint16_t(record[4]) << 8);
            if (x < 320 && y < 480) {
                // Raw portrait 320x480 -> ST7796 MV landscape 480x320.
                // Match rotation 1: swap axes without mirroring screen X.
                point.x = y;
                point.y = x;
                point.pressed = true;
            }
        }
        // Consume release, invalid and multi-touch frames too; only report
        // a press when the controller provides one valid finger.
        m_bus.beginTransmission(m_address);
        m_bus.write(uint8_t(0x81));
        m_bus.write(uint8_t(0x4e));
        m_bus.write(uint8_t(0));
        if (m_bus.endTransmission() != 0) return release();
        m_lastReport = now;
        m_point = point;
        return m_point;
    }

private:
    Bus& m_bus;
    uint8_t m_address = 0;
    uint32_t m_lastReport = 0;
    uint32_t m_lastProbe = 0;
    Point m_point;

    Point release() {
        m_point.pressed = false;
        return m_point;
    }

    bool read(uint16_t reg, uint8_t* data, size_t length) {
        m_bus.beginTransmission(m_address);
        m_bus.write(uint8_t(reg >> 8));
        m_bus.write(uint8_t(reg));
        if (m_bus.endTransmission(false) != 0 ||
            m_bus.requestFrom(m_address, length) != length) return false;
        for (size_t i = 0; i < length; ++i) data[i] = m_bus.read();
        return true;
    }
};
