#pragma once
#include <stdint.h>

static const int MAX_BAN = 32;

struct BanList { uint32_t ip[MAX_BAN]; int n; };

static inline bool banHas(const BanList& b, uint32_t ip) {
  for (int i = 0; i < b.n; i++) if (b.ip[i] == ip) return true;
  return false;
}

static inline bool banAdd(BanList& b, uint32_t ip) {
  if (banHas(b, ip)) return true;
  if (b.n >= MAX_BAN) return false;
  b.ip[b.n++] = ip;
  return true;
}

static inline int banToggle(BanList& b, uint32_t ip) {
  for (int i = 0; i < b.n; i++) if (b.ip[i] == ip) { b.ip[i] = b.ip[--b.n]; return 0; }
  return banAdd(b, ip) ? 1 : -1;
}
