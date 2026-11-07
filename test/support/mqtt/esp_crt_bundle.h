#pragma once
#include <stddef.h>
#include <stdint.h>
extern int testCaResult;
inline int esp_crt_bundle_set(const uint8_t*,size_t) { return testCaResult; }
