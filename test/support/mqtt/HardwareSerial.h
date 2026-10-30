#pragma once
#include "Arduino.h"
#include <deque>
#include <functional>
#include <algorithm>
#include <vector>

class HardwareSerial {
public:
    static HardwareSerial* latest;
    static std::function<void(HardwareSerial&, uint8_t)> written;
    bool failWrites = false;
    int failCommand = -1;
    std::deque<uint8_t> bytes;
    struct Arrival { uint32_t at; uint8_t value; };
    std::deque<Arrival> arrivals;
    explicit HardwareSerial(int) { latest = this; }
    void begin(uint32_t, int, uint8_t, uint8_t) {}
    size_t write(uint8_t value) {
        if (failWrites || value == failCommand) return 0;
        if (written) written(*this, value);
        return 1;
    }
    void schedule(uint32_t afterMs, const std::vector<uint8_t>& values, uint32_t spacingMs = 0) {
        uint32_t at = millis() + afterMs;
        for (uint8_t value : values) {
            auto pos = std::upper_bound(arrivals.begin(), arrivals.end(), at,
                [](uint32_t time, const Arrival& item) { return int32_t(time - item.at) < 0; });
            arrivals.insert(pos, Arrival{at, value});
            at += spacingMs;
        }
    }
    int available() {
        while (!arrivals.empty() && int32_t(millis() - arrivals.front().at) >= 0) {
            bytes.push_back(arrivals.front().value);
            arrivals.pop_front();
        }
        return bytes.size();
    }
    int read() { const auto value = bytes.front(); bytes.pop_front(); return value; }
};
