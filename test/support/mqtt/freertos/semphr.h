#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = void*;
extern unsigned testMutexTakes;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return reinterpret_cast<void*>(1); }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) { ++testMutexTakes; return pdTRUE; }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t) {}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
