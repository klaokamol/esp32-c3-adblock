#pragma once
#include <ctype.h>
#include <string.h>

static inline bool hasCtl(const char* s) {
  for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return true;
  return false;
}

static inline size_t normDomain(char* s) {
  size_t n = strlen(s), a = 0;
  while (a < n && isspace((unsigned char)s[a])) a++;
  while (n > a && isspace((unsigned char)s[n - 1])) n--;
  n -= a;
  memmove(s, s + a, n);
  s[n] = 0;
  for (size_t i = 0; i < n; i++) s[i] = tolower((unsigned char)s[i]);
  if (strncmp(s, "www.", 4) == 0) { n -= 4; memmove(s, s + 4, n + 1); }
  return n;
}
