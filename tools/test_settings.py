import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"


def run(body):
    harness = '#include "settings.h"\n#include "board_ui.h"\nint main() {\n  long v[S_COUNT]; settingsDefaults(v);\n' + body + "\n}\n"
    with tempfile.TemporaryDirectory() as d:
        src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
        src.write_text(harness)
        subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()


class Settings(unittest.TestCase):
    def test_round_trip(self):
        out = run(r'''
  long w[S_COUNT]; settingsDefaults(w);
  long set[S_COUNT] = {6, 0x15, 4, 2, 23, 6, -5, 20393};
  for (int i = 0; i < S_COUNT; i++) { char b[32]; settingFormat(i, set[i], b, sizeof b); settingParse(b, w); }
  for (int i = 0; i < S_COUNT; i++) printf("%ld ", w[i]);
''')
        self.assertEqual(out, ["6", "21", "4", "2", "23", "6", "-5", "20393"])

    def test_missing_keys_keep_defaults(self):
        out = run(r'''
  settingParse("tz=9", v);
  for (int i = 0; i < S_COUNT; i++) printf("%ld ", v[i]);
''')
        self.assertEqual(out, ["24", "0", "3", "0", "22", "7", "9", "-1"])

    def test_unknown_and_garbage_lines_ignored(self):
        out = run(r'''
  printf("%d %d %d %d %d\n", settingParse("newkey=5", v), settingParse("upiv", v), settingParse("upiv=", v), settingParse("up=1", v), settingParse("upivx=3", v));
  printf("%ld\n", v[S_UPIV]);
''')
        self.assertEqual(out, ["-1", "-1", "-1", "-1", "-1", "24"])

    def test_out_of_range_clamps(self):
        out = run(r'''
  settingParse("upiv=0", v); settingParse("uphour=99", v); settingParse("tz=-20", v); settingParse("nmode=5", v);
  printf("%ld %ld %ld %ld\n", v[S_UPIV], v[S_UPHOUR], v[S_TZ], v[S_NMODE]);
''')
        self.assertEqual(out, ["1", "23", "-12", "2"])

    def test_persisted_last_day_blocks_a_second_fetch_that_day(self):
        out = run(r'''
  settingParse("lastday=20393", v);
  int days = 1 << 2;
  printf("%d %d %d\n", updateDue(days, 2, 5, 3, 20393, v[S_LASTDAY]), updateDue(days, 2, 5, 3, 20400, v[S_LASTDAY]), updateDue(days, 2, 5, 3, 20393, SETTINGS[S_LASTDAY].def));
''')
        self.assertEqual(out, ["0", "1", "1"])


if __name__ == "__main__":
    unittest.main()
