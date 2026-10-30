#pragma once
#include <stdint.h>
class IPAddress {
public:
    IPAddress(uint32_t value = 0) : m_value(value) {}
    bool operator==(const IPAddress& other) const { return m_value == other.m_value; }
private:
    uint32_t m_value;
};
