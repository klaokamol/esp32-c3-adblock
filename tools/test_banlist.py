import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"


def run(body):
    harness = '#include <stdio.h>\n#include "banlist.h"\nint main() {\n  static BanList b = {};\n' + body + "\n}\n"
    with tempfile.TemporaryDirectory() as d:
        src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
        src.write_text(harness)
        subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split()


class BanListTest(unittest.TestCase):
    def test_toggle_one_ip_keeps_the_others(self):
        out = run(r'''
  banAdd(b, 1); banAdd(b, 2); banAdd(b, 3);
  printf("%d\n", banToggle(b, 9));
  printf("%d\n", banToggle(b, 9));
  printf("%d%d%d%d %d\n", banHas(b, 1), banHas(b, 2), banHas(b, 3), banHas(b, 9), b.n);
''')
        self.assertEqual(out, ["1", "0", "1110", "3"])

    def test_unban_removes_only_that_ip(self):
        out = run(r'''
  banAdd(b, 1); banAdd(b, 2); banAdd(b, 3);
  printf("%d\n", banToggle(b, 2));
  printf("%d%d%d %d\n", banHas(b, 1), banHas(b, 2), banHas(b, 3), b.n);
''')
        self.assertEqual(out, ["0", "101", "2"])

    def test_add_is_idempotent(self):
        out = run(r'''
  banAdd(b, 7); banAdd(b, 7);
  printf("%d\n", b.n);
''')
        self.assertEqual(out, ["1"])

    def test_full_list_refuses_new_but_still_unbans(self):
        out = run(r'''
  for (int i = 0; i < MAX_BAN; i++) banAdd(b, 100 + i);
  printf("%d %d\n", banAdd(b, 1), banToggle(b, 1));
  printf("%d %d\n", banToggle(b, 100), banToggle(b, 1));
  printf("%d%d %d\n", banHas(b, 100), banHas(b, 1), b.n);
''')
        self.assertEqual(out, ["0", "-1", "0", "1", "01", str(32)])


if __name__ == "__main__":
    unittest.main()
