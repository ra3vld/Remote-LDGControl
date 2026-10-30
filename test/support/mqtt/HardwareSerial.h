#pragma once
#include "Arduino.h"
#include <deque>
#include <functional>

class HardwareSerial {
public:
    static HardwareSerial* latest;
    static std::function<void(HardwareSerial&, uint8_t)> written;
    bool failWrites = false;
    std::deque<uint8_t> bytes;
    explicit HardwareSerial(int) { latest = this; }
    void begin(uint32_t, int, uint8_t, uint8_t) {}
    size_t write(uint8_t value) {
        if (failWrites) return 0;
        if (written) written(*this, value);
        return 1;
    }
    int available() { return bytes.size(); }
    int read() { const auto value = bytes.front(); bytes.pop_front(); return value; }
};
