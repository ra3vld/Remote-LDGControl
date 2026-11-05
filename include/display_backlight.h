#pragma once
#include <atomic>
#include "brightness_control.h"

class DisplayBacklight {
public:
    void begin();
    void loop();
    void writeStatus(JsonObject doc) const;
    static bool supported() {
#ifdef BOARD_ESP32_3248S035C
        return true;
#else
        return false;
#endif
    }
private:
    ldg::BrightnessControl m_control;
    std::atomic<uint32_t> m_sample{0};
    bool m_ready = false;
    uint32_t m_lastRead = 0;
    uint8_t m_duty = 255;
};
extern DisplayBacklight displayBacklight;
