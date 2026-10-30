#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
using String = std::string;
using byte = uint8_t;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
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
