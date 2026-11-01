import gzip
import pathlib
import re


def page_html(root):
    src = (root / "src" / "page.h").read_text(encoding="utf-8")
    return re.search(r'R"HTML\((.*)\)HTML"', src, re.S).group(1).encode("utf-8")


def page_gz(root):
    return gzip.compress(page_html(root), 9, mtime=0)


def write(root):
    gz = page_gz(root)
    rows = ",\n".join(", ".join(str(b) for b in gz[i:i + 24]) for i in range(0, len(gz), 24))
    text = "#pragma once\n#include <stdint.h>\n\nstatic const uint8_t PAGE_GZ[] = {\n" + rows + "\n};\n"
    out = root / "src" / "page_gz.h"
    if not out.exists() or out.read_text() != text:
        out.write_text(text)


if __name__ == "__main__":
    write(pathlib.Path(__file__).resolve().parent.parent)
elif "Import" in globals():
    Import("env")
    write(pathlib.Path(env["PROJECT_DIR"]))
