import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
HARNESS = r'''
#include <stddef.h>
#include "board_ui.h"
int main() {
  char o[16];
  unsigned long c[] = {0, 999, 1000, 1250, 9999, 10000, 999999, 1000000, 8421000, 10000000, 999999999, 4294967295UL};
  for (unsigned long v : c) { fmtCount(o, sizeof o, v); printf("%s\n", o); }
  unsigned long u[] = {0, 3599, 3600, 172799, 172800, 99999999};
  for (unsigned long v : u) { fmtUptime(o, sizeof o, v); printf("%s\n", o); }
  printf("%d%d%d%d%d%d%d\n", ledOn(5000, true, false), ledOn(0, false, false), ledOn(250, false, false), ledOn(0, false, true), ledOn(100, false, true), ledOn(5000, false, true), ledOn(4999, false, true));
  printf("%d%d%d%d\n", screenAwake(59999, 0), screenAwake(60000, 0), screenAwake(100, 4294967000UL), wakeEvent(true, false, false, false) && !wakeEvent(true, true, false, false) && wakeEvent(false, false, true, false));
  int r[] = {-30, -55, -59, -65, -70, -75, -80, -85, -90};
  for (int v : r) printf("%d\n", rssiBars(v));
}
'''


class BoardUi(unittest.TestCase):
    def test_formats(self):
        with tempfile.TemporaryDirectory() as d:
            src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
            src.write_text(HARNESS)
            subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()
        counts = out[:12]
        self.assertEqual(counts, ["0", "999", "1.0k", "1.2k", "9.9k", "10k", "999k", "1.0M", "8.4M", "10M", "999M", "999M"])
        self.assertTrue(all(len(c) <= 4 for c in counts))
        self.assertEqual(out[12:18], ["0m", "59m", "1h", "47h", "2d", "999d"])
        self.assertEqual(out[18], "1101010")
        self.assertEqual(out[19], "1011")
        self.assertEqual(out[20:], ["4", "3", "3", "2", "2", "1", "1", "0", "0"])


if __name__ == "__main__":
    unittest.main()
