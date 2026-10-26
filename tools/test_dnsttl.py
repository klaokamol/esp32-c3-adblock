import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "dnsttl.h"
static const uint8_t Q[] = {0x12,0x34, 0x81,0x80, 0,1, 0,2, 0,0, 0,0, 1,'a',0, 0,1, 0,1};
static const uint8_t A1[] = {0xC0,0x0C, 0,1, 0,1, 0,0,0x01,0x2C, 0,4, 1,2,3,4};
static const uint8_t A2[] = {1,'a',0, 0,1, 0,1, 0,0,0,60, 0,4, 5,6,7,8};
static int build(uint8_t* r) {
  int n = 0;
  memcpy(r + n, Q, sizeof Q); n += sizeof Q;
  memcpy(r + n, A1, sizeof A1); n += sizeof A1;
  memcpy(r + n, A2, sizeof A2); n += sizeof A2;
  return n;
}
static void show(const uint8_t* r, int n) {
  uint32_t t = 0;
  if (dnsMinTtl(r, n, sizeof Q, &t)) printf("%u\n", t); else printf("no\n");
}
int main() {
  uint8_t r[128]; int n = build(r);
  show(r, n);
  r[2] |= 0x02; show(r, n); n = build(r);
  r[3] = 0x83; show(r, n); n = build(r);
  r[7] = 0; show(r, n); n = build(r);
  show(r, n - 1);
  r[sizeof Q + 6] = 0x80; show(r, n);
}
'''


class DnsTtl(unittest.TestCase):
    def test_min_ttl_of_cacheable_answers(self):
        with tempfile.TemporaryDirectory() as d:
            src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
            src.write_text(HARNESS)
            subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ["60", "no", "no", "no", "no", "0"])


if __name__ == "__main__":
    unittest.main()
