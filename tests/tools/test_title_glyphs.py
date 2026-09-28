"""Coordinate/state invariants for bitmap text replacement; no game needed."""

import sys
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from title_glyphs import (
    COUNTER_LABELS,
    KEY_CELLS,
    POL_LABELS,
    STATUS_CELLS,
    TITLE_LABELS,
    refine_pol_atlas,
    refine_status_counter_atlas,
    refine_status_icons_atlas,
    refine_title_atlas,
)

FONT = Path("C:/Windows/Fonts/arialbd.ttf")


def atlas(size, labels):
    """Contrasting ink plus arbitrary untouched padding, independent of assets."""
    width, height = size
    source = np.zeros((height, width, 4), dtype=np.uint8)
    # Nonzero invisible RGB catches accidental changes outside the allowed cells.
    source[:, :, :3] = (13, 19, 23)
    for index, (_, (left, top, right, bottom)) in enumerate(labels):
        color = (230, 210, 170) if index % 2 == 0 else (140, 120, 100)
        source[top + 2 : bottom - 2, left + 1 : right - 1] = (*color, 255)
    baseline = np.array(Image.fromarray(source).resize((width * 4, height * 4)))
    return source, baseline


@unittest.skipUnless(FONT.is_file(), "Local rasterization font is required")
class TitleGlyphTests(unittest.TestCase):
    def test_title_limits_preserve_beta_and_suffix_cells(self):
        source, baseline = atlas((256, 96), TITLE_LABELS)
        source_before, baseline_before = source.copy(), baseline.copy()
        result, audit = refine_title_atlas(source, baseline, FONT)
        np.testing.assert_array_equal(source, source_before)
        np.testing.assert_array_equal(baseline, baseline_before)
        np.testing.assert_array_equal(result[64 * 4 :], baseline[64 * 4 :])
        np.testing.assert_array_equal(
            result[48 * 4 : 64 * 4, 186 * 4 :], baseline[48 * 4 : 64 * 4, 186 * 4 :]
        )
        np.testing.assert_array_equal(
            result[19 * 4 : 24 * 4], baseline[19 * 4 : 24 * 4]
        )
        self.assertEqual(len(audit["regions"]), 14)
        self.assertTrue(np.any(result != baseline))

    def test_original_constructor_version_cells_remain_distinct(self):
        # Actual complete +1480F0 execution captured +18A140 sprite records for
        # Ver.1.06.011. The registered source and destination widths are equal.
        source, baseline = atlas((256, 96), TITLE_LABELS)
        result, _ = refine_title_atlas(source, baseline, FONT)
        records = (
            ((0, 48, 24, 64), (-1, 403, 23, 419)),
            ((48, 48, 56, 64), (23, 403, 31, 419)),
            ((24, 48, 28, 64), (31, 403, 35, 419)),
            ((32, 48, 42, 64), (35, 403, 45, 419)),
            ((128, 48, 137, 64), (45, 403, 54, 419)),
            ((24, 48, 28, 64), (54, 403, 58, 419)),
            ((32, 48, 42, 64), (58, 403, 68, 419)),
            ((48, 48, 56, 64), (68, 403, 76, 419)),
            ((48, 48, 56, 64), (76, 403, 84, 419)),
        )
        for (left, top, right, bottom), (x0, y0, x1, y1) in records:
            self.assertEqual((right - left, bottom - top), (x1 - x0, y1 - y0))
            crop = result[top * 4 : bottom * 4, left * 4 : right * 4]
            self.assertTrue(np.any(crop[:, :, 3] > 128))
            self.assertTrue(np.any(crop[:, :, 3] == 0))

    def test_toolbar_state_variants_keep_distinct_ink_colors(self):
        source, baseline = atlas((200, 168), (*POL_LABELS, *KEY_CELLS))
        result, audit = refine_pol_atlas(source, baseline, FONT)
        # Untouched Chat wording, pad buttons, F4 and each key's extra UV texel.
        for left, top, right, bottom in (
            (144, 24, 200, 115),
            (0, 72, 200, 95),
            (160, 120, 200, 143),
            (80, 162, 177, 164),
        ):
            np.testing.assert_array_equal(
                result[top * 4 : bottom * 4, left * 4 : right * 4],
                baseline[top * 4 : bottom * 4, left * 4 : right * 4],
            )
        for index, (_, (left, top, right, bottom)) in enumerate(POL_LABELS[:2]):
            crop = result[top * 4 : bottom * 4, left * 4 : right * 4]
            ink = (230, 210, 170) if index == 0 else (140, 120, 100)
            self.assertTrue(np.any(np.all(crop[:, :, :3] == ink, axis=2)))
            orange = (
                (crop[:, :, 0] > 200) & (crop[:, :, 1] < 180) & (crop[:, :, 2] < 60)
            )
            self.assertEqual(bool(np.any(orange)), index == 1)
        self.assertEqual(len(audit["regions"]), 9)

    def test_icons_preserve_adjacent_statuses_and_extra_sampled_row(self):
        source, baseline = atlas((256, 96), STATUS_CELLS)
        result, audit = refine_status_icons_atlas(source, baseline)
        np.testing.assert_array_equal(result[: 48 * 4], baseline[: 48 * 4])
        np.testing.assert_array_equal(result[80 * 4 :], baseline[80 * 4 :])
        np.testing.assert_array_equal(result[:, 72 * 4 :], baseline[:, 72 * 4 :])
        for _, (left, top, right, bottom) in STATUS_CELLS:
            crop = result[top * 4 : bottom * 4, left * 4 : right * 4]
            # Clear outline and antialiased coverage both survive rasterization.
            self.assertTrue(np.any(crop[:, :, 3] == 255))
            self.assertTrue(np.any((crop[:, :, 3] > 0) & (crop[:, :, 3] < 255)))
        self.assertEqual(len(audit["regions"]), 2)

    def test_status_counter_preserves_unknown_marker_and_other_statuses(self):
        source, baseline = atlas((256, 48), COUNTER_LABELS)
        result, _ = refine_status_counter_atlas(source, baseline, FONT)
        np.testing.assert_array_equal(result[:, : 56 * 4], baseline[:, : 56 * 4])
        np.testing.assert_array_equal(result[:, 214 * 4 :], baseline[:, 214 * 4 :])
        np.testing.assert_array_equal(result[17 * 4 :], baseline[17 * 4 :])
        for digit in range(10):
            # Two untouched texels separate successive 14-texel sampled cells.
            left = (70 + digit * 16) * 4
            np.testing.assert_array_equal(
                result[:, left : left + 8], baseline[:, left : left + 8]
            )

    def test_wrong_atlas_or_nonuniform_scale_is_rejected(self):
        source, baseline = atlas((256, 96), TITLE_LABELS)
        for wrong_source, wrong_base in (
            (source[:, :-1], baseline),
            (source, baseline[:-1]),
            (source.astype(float), baseline),
        ):
            with self.subTest(shape=wrong_source.shape), self.assertRaises(ValueError):
                refine_title_atlas(wrong_source, wrong_base, FONT)

    def test_blank_cell_fails_before_returning_incomplete_pack(self):
        source, baseline = atlas((256, 96), TITLE_LABELS)
        source[48:64, 48:56] = 0  # Missing mapped digit1.
        with self.assertRaisesRegex(ValueError, "No original ink"):
            refine_title_atlas(source, baseline, FONT)


if __name__ == "__main__":
    unittest.main()
