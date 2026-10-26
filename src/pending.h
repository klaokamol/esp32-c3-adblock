#pragma once
#include <stdint.h>
#include <string.h>

static const int PEND_PKT = 512;

struct PendingQuery {
  bool used;
  uint16_t wid, cid, len, qend, cport;
  uint8_t up, sentTo, tries;
  uint64_t key;
  uint32_t cip, sentAt;
  uint8_t pkt[PEND_PKT];
};

static inline int pendFree(const PendingQuery* t, int n) {
  for (int i = 0; i < n; i++) if (!t[i].used) return i;
  return -1;
}

static inline bool pendWidUsed(const PendingQuery* t, int n, uint16_t wid) {
  for (int i = 0; i < n; i++) if (t[i].used && t[i].wid == wid) return true;
  return false;
}

static inline int pendMatch(const PendingQuery* t, int n, uint8_t up, const uint8_t* r, int len) {
  if (len < 12) return -1;
  const uint16_t wid = (uint16_t)(r[0] << 8 | r[1]);
  for (int i = 0; i < n; i++) {
    const PendingQuery& p = t[i];
    if (!p.used || p.wid != wid || !(p.sentTo >> up & 1) || len < p.qend) continue;
    if (memcmp(r + 12, p.pkt + 12, p.qend - 12) == 0) return i;
  }
  return -1;
}

static inline int pendExpired(const PendingQuery* t, int n, uint32_t now, uint32_t timeout) {
  for (int i = 0; i < n; i++) if (t[i].used && now - t[i].sentAt >= timeout) return i;
  return -1;
}
