import re
from pathlib import Path

PAGE_H = Path(__file__).resolve().parent.parent / "src" / "page.h"
BUDGET = 16 * 1024


def page_bytes():
    src = PAGE_H.read_text(encoding="utf-8")
    return re.search(r'R"HTML\((.*)\)HTML"', src, re.S).group(1).encode("utf-8")


def test_dashboard_fits_flash_budget():
    size = len(page_bytes())
    assert size <= BUDGET, f"dashboard is {size} B, budget {BUDGET} B"


if __name__ == "__main__":
    test_dashboard_fits_flash_budget()
    print(f"ok: {len(page_bytes())} B")
