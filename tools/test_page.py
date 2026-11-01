import pathlib
import unittest

from gzip_page import page_gz

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUDGET = 8 * 1024


class Page(unittest.TestCase):
    def test_gzipped_dashboard_fits_flash_budget(self):
        size = len(page_gz(ROOT))
        self.assertLessEqual(size, BUDGET, f"gzipped dashboard is {size} B, budget {BUDGET} B")

    def test_gzip_is_deterministic(self):
        self.assertEqual(page_gz(ROOT), page_gz(ROOT))


if __name__ == "__main__":
    unittest.main()
