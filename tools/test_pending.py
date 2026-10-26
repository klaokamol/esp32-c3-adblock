import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"


def run(body):
    harness = '#include <stdio.h>\n#include "pending.h"\nint main() {\n' + body + "\n}\n"
    with tempfile.TemporaryDirectory() as d:
        src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
        src.write_text(harness)
        subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()


class Pending(unittest.TestCase):
    def test_free_slot_until_full(self):
        out = run(r'''
  static PendingQuery t[3] = {};
  printf("%d\n", pendFree(t, 3));
  t[0].used = true;
  printf("%d\n", pendFree(t, 3));
  t[1].used = t[2].used = true;
  printf("%d\n", pendFree(t, 3));
''')
        self.assertEqual(out, ["0", "1", "-1"])

    def test_wid_in_use_only_counts_live_slots(self):
        out = run(r'''
  static PendingQuery t[2] = {};
  t[0].used = true; t[0].wid = 0x1234;
  t[1].used = false; t[1].wid = 0x5678;
  printf("%d%d%d\n", pendWidUsed(t, 2, 0x1234), pendWidUsed(t, 2, 0x5678), pendWidUsed(t, 2, 0x9999));
''')
        self.assertEqual(out, ["100"])

    def test_reply_matches_only_its_own_query(self):
        out = run(r'''
  static PendingQuery t[2] = {};
  const uint8_t qa[] = {0xAB,0xCD, 1,0, 0,1, 0,0, 0,0, 0,0, 1,'a',0, 0,1, 0,1};
  const uint8_t qb[] = {0x22,0x22, 1,0, 0,1, 0,0, 0,0, 0,0, 1,'b',0, 0,1, 0,1};
  t[0].used = true; t[0].wid = 0xABCD; t[0].up = 0; t[0].sentTo = 1 << 0; t[0].qend = sizeof qa; memcpy(t[0].pkt, qa, sizeof qa);
  t[1].used = true; t[1].wid = 0x2222; t[1].up = 1; t[1].sentTo = 1 << 1; t[1].qend = sizeof qb; memcpy(t[1].pkt, qb, sizeof qb);
  uint8_t r[64]; memcpy(r, qa, sizeof qa); r[2] = 0x81; r[3] = 0x80;
  printf("%d\n", pendMatch(t, 2, 0, r, sizeof qa + 16));
  printf("%d\n", pendMatch(t, 2, 1, r, sizeof qa + 16));
  printf("%d\n", pendMatch(t, 2, 0, r, sizeof qa - 1));
  r[13] = 'b';
  printf("%d\n", pendMatch(t, 2, 0, r, sizeof qa));
  memcpy(r, qb, sizeof qb);
  printf("%d\n", pendMatch(t, 2, 1, r, sizeof qb));
  t[1].used = false;
  printf("%d\n", pendMatch(t, 2, 1, r, sizeof qb));
''')
        self.assertEqual(out, ["0", "-1", "-1", "-1", "1", "-1"])

    def test_late_reply_from_first_upstream_still_matches_after_failover(self):
        out = run(r'''
  static PendingQuery t[1] = {};
  const uint8_t q[] = {0xAB,0xCD, 1,0, 0,1, 0,0, 0,0, 0,0, 1,'a',0, 0,1, 0,1};
  t[0].used = true; t[0].wid = 0xABCD; t[0].up = 1; t[0].sentTo = 1 << 0 | 1 << 1; t[0].qend = sizeof q; memcpy(t[0].pkt, q, sizeof q);
  printf("%d%d\n", pendMatch(t, 1, 0, q, sizeof q), pendMatch(t, 1, 1, q, sizeof q));
  t[0].sentTo = 1 << 1;
  printf("%d\n", pendMatch(t, 1, 0, q, sizeof q));
''')
        self.assertEqual(out, ["00", "-1"])

    def test_expired_slot_survives_millis_wrap(self):
        out = run(r'''
  static PendingQuery t[3] = {};
  t[0].used = true; t[0].sentAt = 1000;
  t[2].used = false; t[2].sentAt = 0;
  printf("%d\n", pendExpired(t, 3, 1499, 500));
  printf("%d\n", pendExpired(t, 3, 1500, 500));
  t[0].used = false;
  t[1].used = true; t[1].sentAt = 4294967000UL;
  printf("%d\n", pendExpired(t, 3, 203, 500));
  printf("%d\n", pendExpired(t, 3, 204, 500));
''')
        self.assertEqual(out, ["-1", "0", "-1", "1"])


if __name__ == "__main__":
    unittest.main()
