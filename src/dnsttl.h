#pragma once
#include <stdint.h>

static inline int dnsSkipName(const uint8_t* r, int n, int i) {
  while (i < n) {
    const uint8_t l = r[i];
    if (l == 0) return i + 1;
    if ((l & 0xC0) == 0xC0) return i + 2 <= n ? i + 2 : -1;
    if (l & 0xC0) return -1;
    i += l + 1;
  }
  return -1;
}

static inline bool dnsMinTtl(const uint8_t* r, int n, int qend, uint32_t* ttl) {
  if (n < 12 || (r[2] & 0x02) || (r[3] & 0x0F)) return false;
  const int an = r[6] << 8 | r[7];
  if (!an) return false;
  int i = qend;
  for (int k = 0; k < an; k++) {
    i = dnsSkipName(r, n, i);
    if (i < 0 || i + 10 > n) return false;
    uint32_t t = (uint32_t)r[i + 4] << 24 | (uint32_t)r[i + 5] << 16 | (uint32_t)r[i + 6] << 8 | r[i + 7];
    if (t & 0x80000000u) t = 0;
    i += 10 + (r[i + 8] << 8 | r[i + 9]);
    if (i > n) return false;
    if (k == 0 || t < *ttl) *ttl = t;
  }
  return true;
}
