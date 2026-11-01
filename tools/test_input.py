import pathlib
import subprocess
import tempfile
import unittest

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"


def run(body):
    harness = '#include <stdio.h>\n#include "input.h"\nint main() {\n' + body + "\n}\n"
    with tempfile.TemporaryDirectory() as d:
        src, exe = pathlib.Path(d, "t.cpp"), pathlib.Path(d, "t")
        src.write_text(harness)
        subprocess.run(["c++", "-std=c++11", "-I", str(SRC), str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()


class Input(unittest.TestCase):
    def test_control_characters(self):
        out = run(r'''
  const char* c[] = {"ads.example.com", "a.com\nb.com", "a\rb", "tab\there", "del\x7f", "https://x.y/blocklist.bin?a=1&b=2", ""};
  for (const char* s : c) printf("%d", hasCtl(s));
  printf("\n");
''')
        self.assertEqual(out, ["0111100"])

    def test_normalise_domain(self):
        out = run(r'''
  const char* c[] = {"  WWW.Example.COM \r", "ads.example.com", "www.", "Sub.WWW.example.com", "", "   "};
  for (const char* s : c) { char b[64]; strcpy(b, s); size_t n = normDomain(b); printf("[%s]%zu\n", b, n); }
''')
        self.assertEqual(out, ["[example.com]11", "[ads.example.com]15", "[]0", "[sub.www.example.com]19", "[]0", "[]0"])

    def test_normalise_is_idempotent(self):
        out = run(r'''
  char b[64] = " WWW.Ads.Example.com";
  normDomain(b); char c[64]; strcpy(c, b); normDomain(c);
  printf("%d\n", strcmp(b, c) == 0);
''')
        self.assertEqual(out, ["1"])


if __name__ == "__main__":
    unittest.main()
