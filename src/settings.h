#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_UPIV, S_UPDAYS, S_UPHOUR, S_NMODE, S_NFROM, S_NTO, S_TZ, S_LASTDAY, S_COUNT };

struct SettingDef { const char* key; long def, lo, hi; };

static const SettingDef SETTINGS[S_COUNT] = {
  {"upiv", 24, 1, 168},
  {"updays", 0, 0, 127},
  {"uphour", 3, 0, 23},
  {"nmode", 0, 0, 2},
  {"nfrom", 22, 0, 23},
  {"nto", 7, 0, 23},
  {"tz", 7, -12, 14},
  {"lastday", -1, -1, 1000000},
};

static inline long settingClamp(int i, long v) {
  const SettingDef& d = SETTINGS[i];
  return v < d.lo ? d.lo : v > d.hi ? d.hi : v;
}

static inline void settingsDefaults(long* v) {
  for (int i = 0; i < S_COUNT; i++) v[i] = SETTINGS[i].def;
}

static inline int settingParse(const char* line, long* v) {
  const char* eq = strchr(line, '=');
  if (!eq) return -1;
  size_t kl = eq - line;
  for (int i = 0; i < S_COUNT; i++) {
    if (strlen(SETTINGS[i].key) != kl || strncmp(line, SETTINGS[i].key, kl)) continue;
    char* end;
    long x = strtol(eq + 1, &end, 10);
    if (end == eq + 1) return -1;
    v[i] = settingClamp(i, x);
    return i;
  }
  return -1;
}

static inline int settingFormat(int i, long v, char* out, size_t n) {
  return snprintf(out, n, "%s=%ld", SETTINGS[i].key, v);
}
