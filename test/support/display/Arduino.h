#pragma once
#ifdef __cplusplus
#include "../mqtt/Arduino.h"
inline long map(long value,long fromLow,long fromHigh,long toLow,long toHigh) {
    return (value-fromLow)*(toHigh-toLow)/(fromHigh-fromLow)+toLow;
}
#else
#include <stdint.h>
uint32_t display_test_millis(void);
#define millis display_test_millis
#endif
