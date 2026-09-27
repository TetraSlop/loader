"""Cursor reconstruction layout checks without proprietary image fixtures."""

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cursor_art import CELL_SIZE, INK_BOUNDS, vector_cursor


def native_layout():
    source = np.zeros((40, 120, 4), dtype=np.uint8)
    for pose, (left, top, right, bottom) in enumerate(INK_BOUNDS):
        source[top:bottom, pose * CELL_SIZE + left : pose * CELL_SIZE + right] = 255
    return source


class CursorArtTests(unittest.TestCase):
    def test_native_cells_and_transparent_gutters_are_preserved(self):
        source = native_layout()
        for scale in (1, 4, 8):
            with self.subTest(scale=scale):
                result, audit = vector_cursor(source, scale)
                self.assertEqual(result.shape, (40 * scale, 120 * scale, 4))
                self.assertEqual(result.dtype, np.uint8)
                for pose, bounds in enumerate(INK_BOUNDS):
                    cell = result[:, pose * 40 * scale : (pose + 1) * 40 * scale]
                    left, top, right, bottom = (value * scale for value in bounds)
                    allowed = np.zeros(cell.shape[:2], dtype=bool)
                    allowed[top:bottom, left:right] = True
                    self.assertFalse(cell[~allowed].any())
                    self.assertEqual(
                        audit["poses"][pose]["output_bounds"],
                        [left, top, right, bottom],
                    )
                    self.assertGreater(
                        np.count_nonzero(cell[:, :, 3] == 255), 200 * scale**2
                    )
                self.assertTrue(np.array_equal(source, native_layout()))

    def test_duplicate_atlases_generate_identical_pixels(self):
        source = native_layout()
        first, first_audit = vector_cursor(source)
        duplicate, duplicate_audit = vector_cursor(source.copy())
        self.assertTrue(np.array_equal(first, duplicate))
        self.assertEqual(first_audit, duplicate_audit)

    def test_unknown_layout_or_scale_is_rejected(self):
        source = native_layout()
        with self.assertRaisesRegex(ValueError, "120x40"):
            vector_cursor(source[:, :40])
        shifted = np.roll(source, 1, axis=1)
        with self.assertRaisesRegex(ValueError, "bounds"):
            vector_cursor(shifted)
        for scale in (0, 9, 1.5, True):
            with self.subTest(scale=scale), self.assertRaisesRegex(ValueError, "scale"):
                vector_cursor(source, scale)


if __name__ == "__main__":
    unittest.main()
