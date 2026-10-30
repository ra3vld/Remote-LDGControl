#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <cmath>
#include <cstring>
using std::abs;
using std::sqrt;
using String = std::string;
using byte = uint8_t;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }
inline void esp_fill_random(void* buffer, size_t length) { memset(buffer, 0x5a, length); }
#define SERIAL_8N1 0
#define INPUT 0
#define ADC_11db 3
extern int testAnalogValue;
extern int testAnalogReads;
inline void pinMode(int, int) {}
inline void analogReadResolution(int) {}
inline void analogSetAttenuation(int) {}
inline int analogRead(int) { ++testAnalogReads; return testAnalogValue; }
struct FakeSerial {
    std::string output;
    void print(const char* text) { output += text; }
    void println() { output += '\n'; }
    void printf(const char* format, ...) {
        char buffer[768];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        output += buffer;
    }
};
extern FakeSerial Serial;
