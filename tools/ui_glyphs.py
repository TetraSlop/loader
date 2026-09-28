"""Redraw main-menu labels and on_card1 stat sprites as vector art inside their native rectangles.

Pixels outside the audited rectangles stay byte-identical to the 4x baseline.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from title_glyphs import render_text

# Seven main-menu entries at TM +14899D..148B69, decoded by +18EB40 and drawn as 120x24 at +18EDD4/+18EEF2.
# VS. COM is also drawn as 120x24 from (0, 72), at +BD25D..BD286. See docs/ui-asset-plan.md.
MENU_LABELS = (
    ("Play", (120, 48, 240, 72)),
    ("VS. COM", (0, 72, 120, 96)),
    ("Card Shop", (120, 72, 240, 96)),
    ("Auction", (120, 96, 240, 120)),
    ("Player Data", (120, 144, 240, 168)),
    ("Rankings", (120, 168, 240, 192)),
    ("Options", (120, 192, 240, 216)),
    ("Back", (120, 216, 240, 240)),
)
# First eight on_card1 arrows in native order: N, NE, E, SE, S, SW, W, NW, as unit-square triangles.
ARROWS = (
    ((0.5, 0), (0, 1), (1, 1)),
    ((0, 0), (1, 0), (1, 1)),
    ((0, 0), (1, 0.5), (0, 1)),
    ((1, 0), (1, 1), (0, 1)),
    ((0, 0), (1, 0), (0.5, 1)),
    ((0, 0), (0, 1), (1, 1)),
    ((1, 0), (0, 0.5), (1, 1)),
    ((0, 0), (1, 0), (0, 1)),
)


def _gold_bounds(source: np.ndarray, rect: tuple[int, int, int, int], red: int, green: int, blue: int) -> tuple:
    """Return (x, y, width, height) of source pixels with R > red, G > green, B < blue inside rect."""
    left, top, right, bottom = rect
    crop = source[top:bottom, left:right]
    gold = (crop[:, :, 0] > red) & (crop[:, :, 1] > green) & (crop[:, :, 2] < blue) & (crop[:, :, 3] > 0)
    ys, xs = np.where(gold)
    if not len(xs):
        raise ValueError(f"No original gold pixels in {rect}")
    return int(xs.min() + left), int(ys.min() + top), int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)


def _visible_check(kind: str, index: int, rect: tuple[int, int, int, int], patch: Image.Image) -> dict:
    """Audit record for a redrawn sprite; its outline must not touch the cell edge."""
    ys, xs = np.where(np.array(patch)[:, :, 3] > 0)
    check = {
        "kind": kind,
        "index": index,
        "native": list(rect),
        "visible": [int(xs.min()), int(ys.min()), int(xs.max() + 1), int(ys.max() + 1)],
    }
    if xs.min() <= 0 or ys.min() <= 0 or xs.max() >= patch.width - 1 or ys.max() >= patch.height - 1:
        raise ValueError(f"{kind.capitalize()} outline exceeds native gutter: {check}")
    return check


def menu_labels(source: np.ndarray, result: np.ndarray, font_file: Path) -> np.ndarray:
    """Retype the eight menu labels inside their original ink extents."""
    canvas = Image.fromarray(result)
    font = ImageFont.truetype(str(font_file), 256)
    changed = np.zeros(result.shape[:2], bool)
    for label, rect in MENU_LABELS:
        left, top, right, bottom = rect
        x, y, width, height = _gold_bounds(source, rect, 160, 80, 150)
        glyph = render_text(font, label, (width * 4, height * 4))
        # The 1px drop shadow must also stay inside the label's atlas rectangle.
        if x < left + 1 or y < top + 1 or x + width + 1 > right - 1 or y + height + 1 > bottom - 1:
            raise ValueError(f"Main-menu shadow exceeds original rectangle: {label}")
        canvas.paste((0, 0, 0, 0), (left * 4, top * 4, right * 4, bottom * 4))
        shadow = Image.new("RGBA", glyph.size, (53, 27, 5, 255))
        shadow.putalpha(glyph)
        canvas.alpha_composite(shadow, (x * 4 + 4, y * 4 + 4))
        ink = np.zeros((glyph.height, glyph.width, 4), np.uint8)
        for row in range(glyph.height):
            t = row / max(1, glyph.height - 1)
            ink[row, :, :3] = (255, round(210 - 42 * t), round(71 - 48 * t))
        ink[:, :, 3] = np.array(glyph)
        canvas.alpha_composite(Image.fromarray(ink), (x * 4, y * 4))
        changed[top * 4 : bottom * 4, left * 4 : right * 4] = True
    final = np.array(canvas)
    if not np.array_equal(final[~changed], result[~changed]):
        raise ValueError("Menu lettering changed pixels outside its audited rectangles")
    return final


def vector_stats(source: np.ndarray, baseline: np.ndarray, font_file: Path) -> tuple[np.ndarray, list[dict]]:
    """Rebuild the on_card1 direction arrows (+B4F58) and yellow digits (+B5CEA).

    Arrow UVs start one pixel after each 16-pixel cell origin; the 24-pixel yellow digits start at y=104.
    """
    canvas = Image.fromarray(baseline)
    font = ImageFont.truetype(str(font_file), 256)
    changed = np.zeros(baseline.shape[:2], bool)
    checks = []
    for i, vertices in enumerate(ARROWS):
        rect = left, top, right, bottom = i * 16 + 1, 89, i * 16 + 17, 105
        x, y, width, height = _gold_bounds(source, rect, 170, 70, 130)
        # Supersample a clean triangle with the source's dark border.
        scale = 16
        patch = Image.new("RGBA", (16 * scale, 16 * scale))
        draw = ImageDraw.Draw(patch)
        points = [
            ((x - left + (width - 1) * u + 0.5) * scale, (y - top + (height - 1) * v + 0.5) * scale)
            for u, v in vertices
        ]
        draw.line(points + [points[0]], fill=(11, 11, 11, 255), width=3 * scale, joint="curve")
        draw.polygon(points, fill=(232, 159, 25, 255))
        # Darker gold on the lowest edge, as in the source bevel.
        edge = max(zip(points, points[1:] + points[:1]), key=lambda e: (e[0][1] + e[1][1]) / 2)
        draw.line(edge, fill=(192, 95, 1, 255), width=scale // 2)
        patch = patch.resize((64, 64), Image.Resampling.LANCZOS)
        canvas.paste((0, 0, 0, 0), (left * 4, top * 4, right * 4, bottom * 4))
        canvas.alpha_composite(patch, (left * 4, top * 4))
        changed[top * 4 : bottom * 4, left * 4 : right * 4] = True
        checks.append(_visible_check("arrow", i, rect, patch))
    for i in range(10):
        rect = left, top, right, bottom = i * 24, 104, (i + 1) * 24, 128
        x, y, width, height = _gold_bounds(source, rect, 200, 150, 160)
        mask = Image.new("L", (96, 96))
        mask.paste(render_text(font, str(i), (width * 4, height * 4)), ((x - left) * 4, (y - top) * 4))
        shadow = Image.new("L", (96, 96))
        shadow.paste(mask.filter(ImageFilter.MaxFilter(7)), (2, 2))
        patch = Image.new("RGBA", (96, 96))
        background = Image.new("RGBA", (96, 96), (20, 10, 3, 255))
        background.putalpha(shadow)
        patch.alpha_composite(background)
        ink = Image.new("RGBA", (96, 96), (246, 217, 57, 255))
        ink.putalpha(mask)
        patch.alpha_composite(ink)
        canvas.paste((0, 0, 0, 0), (left * 4, top * 4, right * 4, bottom * 4))
        canvas.alpha_composite(patch, (left * 4, top * 4))
        changed[top * 4 : bottom * 4, left * 4 : right * 4] = True
        checks.append(_visible_check("digit", i, rect, patch))
    result = np.array(canvas)
    if not np.array_equal(result[~changed], baseline[~changed]):
        raise ValueError("Stat vectors changed pixels outside their native rectangles")
    return result, checks
