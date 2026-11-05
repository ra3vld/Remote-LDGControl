#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
inline size_t heap_caps_get_free_size(unsigned) { return 120000; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 80000; }
