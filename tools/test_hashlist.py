import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
HARNESS = r'''
#include <stdio.h>
#include "hashlist.h"
int main() {
  const uint8_t up[] = {1,0,0,0,0, 2,0,0,0,0, 2,0,0,0,0, 0,1,0,0,0};
  const uint8_t down[] = {0,1,0,0,0, 1,0,0,0,0};
  uint64_t p = 0, q = 0, r = 0;
  bool split = hashesAscending(up, 10, &r) && hashesAscending(up + 10, 10, &r);
  printf("%d%d%d\n", hashesAscending(up, sizeof up, &p), hashesAscending(down, sizeof down, &q), split);
  uint64_t s = 0x0100000000ULL;
  printf("%d\n", hashesAscending(up, 5, &s));
}
'''


class HashList(unittest.TestCase):
    def test_ascending(self):
        with tempfile.TemporaryDirectory() as d:
            src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
            src.write_text(HARNESS)
            subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ["101", "0"])


if __name__ == "__main__":
    unittest.main()
