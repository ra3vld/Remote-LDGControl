#pragma once
#include <stdint.h>
struct FakeWire {
    void requestFrom(uint8_t,uint8_t) {}
    int available() { return 0; }
    int read() { return 0; }
    void beginTransmission(uint8_t) {}
    void write(uint8_t) {}
    int endTransmission() { return 0; }
};
extern FakeWire Wire;
