#pragma once
#include <stdint.h>
#include <stddef.h>

static inline bool hashesAscending(const uint8_t* b, size_t n, uint64_t* prev) {
  for (size_t i = 0; i + 5 <= n; i += 5) {
    uint64_t v = 0;
    for (int k = 0; k < 5; k++) v |= (uint64_t)b[i + k] << (8 * k);
    if (v < *prev) return false;
    *prev = v;
  }
  return true;
}

static inline bool swapKeepsOld(long incoming, size_t freeBytes, size_t slack) {
  return incoming > 0 && freeBytes >= (size_t)incoming + slack;
}
