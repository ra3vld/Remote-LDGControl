#ifndef VOLTAGE_SENSOR_H
#define VOLTAGE_SENSOR_H

#include <Arduino.h>
#include "config.h"

class VoltageSensor {
public:
    void begin(int8_t pin = VOLTAGE_SENSE_PIN);
    double readVoltage();
    bool isValid() const { return m_valid; }

private:
    int8_t m_pin = -1;
    bool m_valid = false;
};

extern VoltageSensor voltageSensor;

#endif
