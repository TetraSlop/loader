"""Reconstruct reviewed UI glyphs inside verified native sprite rectangles.

The functions accept source RGBA pixels and a 4x baseline. Everything outside
those rectangles stays byte-identical; native layout and state remain in TM.
"""

from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# Exact native source rectangles: seven main-menu entries at TM +14899D..148B69,
# decoded by +18EB40 and drawn as 120x24 at +18EDD4/+18EEF2. VS. COM is also
# drawn as 120x24 from (0,72), at +BD25D..BD286. See docs/ui-asset-plan.md.
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


def menu_labels(source, result, font_file):
    """Retype eight verified labels inside their original ink extents."""
    canvas = Image.fromarray(result)
    font = ImageFont.truetype(str(font_file), 256)
    changed = np.zeros(result.shape[:2], bool)
    for label, (left, top, right, bottom) in MENU_LABELS:
        crop = source[top:bottom, left:right]
        gold = (
            (crop[:, :, 0] > 160)
            & (crop[:, :, 1] > 80)
            & (crop[:, :, 2] < 150)
            & (crop[:, :, 3] > 0)
        )
        ys, xs = np.where(gold)
        if not len(xs):
            raise ValueError(f"Main-menu label has no original gold pixels: {label}")
        x, y = int(xs.min() + left), int(ys.min() + top)
        width, height = int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)
        bounds = font.getbbox(label)
        glyph = Image.new("L", (bounds[2] - bounds[0], bounds[3] - bounds[1]))
        ImageDraw.Draw(glyph).text((-bounds[0], -bounds[1]), label, font=font, fill=255)
        glyph = glyph.crop(glyph.getbbox()).resize(
            (width * 4, height * 4), Image.Resampling.LANCZOS
        )
        # Even the shadow must remain inside the existing label's atlas rectangle.
        if (
            x < left + 1
            or y < top + 1
            or x + width + 1 > right - 1
            or y + height + 1 > bottom - 1
        ):
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


def vector_stats(source, baseline, font_file):
    """Rebuild on_card1 arrows (+B4F58) and yellow digits (+B5CEA).

    Arrow UVs start one pixel after each 16-pixel cell origin. The 24-pixel
    yellow digits start at Y=104; neither formula may be changed by fitting.
    """
    canvas = Image.fromarray(baseline)
    font = ImageFont.truetype(str(font_file), 256)
    changed = np.zeros(baseline.shape[:2], bool)
    checks = []
    # First eight arrows: N, NE, E, SE, S, SW, W, NW in native order.
    triangles = (
        ((0.5, 0), (0, 1), (1, 1)),
        ((0, 0), (1, 0), (1, 1)),
        ((0, 0), (1, 0.5), (0, 1)),
        ((1, 0), (1, 1), (0, 1)),
        ((0, 0), (1, 0), (0.5, 1)),
        ((0, 0), (0, 1), (1, 1)),
        ((1, 0), (0, 0.5), (1, 1)),
        ((0, 0), (1, 0), (0, 1)),
    )
    for i, vertices in enumerate(triangles):
        left, top, right, bottom = i * 16 + 1, 89, i * 16 + 17, 105
        crop = source[top:bottom, left:right]
        gold = (
            (crop[:, :, 0] > 170)
            & (crop[:, :, 1] > 70)
            & (crop[:, :, 2] < 130)
            & (crop[:, :, 3] > 0)
        )
        yy, xx = np.where(gold)
        x, y = int(xx.min() + left), int(yy.min() + top)
        width, height = int(xx.max() - xx.min() + 1), int(yy.max() - yy.min() + 1)
        # Supersample a clean geometric triangle and its original dark border.
        scale = 16
        patch = Image.new("RGBA", (16 * scale, 16 * scale))
        d = ImageDraw.Draw(patch)
        points = [
            (
                (x - left + (width - 1) * u + 0.5) * scale,
                (y - top + (height - 1) * v + 0.5) * scale,
            )
            for u, v in vertices
        ]
        d.line(
            points + [points[0]], fill=(11, 11, 11, 255), width=3 * scale, joint="curve"
        )
        d.polygon(points, fill=(232, 159, 25, 255))
        # The lower bevel remains the source's darker gold, less than one pixel.
        edge = max(
            zip(points, points[1:] + points[:1]), key=lambda e: (e[0][1] + e[1][1]) / 2
        )
        d.line(edge, fill=(192, 95, 1, 255), width=scale // 2)
        patch = patch.resize((64, 64), Image.Resampling.LANCZOS)
        canvas.paste((0, 0, 0, 0), (left * 4, top * 4, right * 4, bottom * 4))
        canvas.alpha_composite(patch, (left * 4, top * 4))
        changed[top * 4 : bottom * 4, left * 4 : right * 4] = True
        alpha = np.array(patch)[:, :, 3]
        yy, xx = np.where(alpha > 0)
        checks.append(
            {
                "kind": "arrow",
                "index": i,
                "native": [left, top, right, bottom],
                "visible": [
                    int(xx.min()),
                    int(yy.min()),
                    int(xx.max() + 1),
                    int(yy.max() + 1),
                ],
            }
        )
        if xx.min() <= 0 or yy.min() <= 0 or xx.max() >= 63 or yy.max() >= 63:
            raise ValueError(f"Arrow outline exceeds native gutter: {checks[-1]}")
    for i in range(10):
        left, top, right, bottom = i * 24, 104, (i + 1) * 24, 128
        crop = source[top:bottom, left:right]
        gold = (
            (crop[:, :, 0] > 200)
            & (crop[:, :, 1] > 150)
            & (crop[:, :, 2] < 160)
            & (crop[:, :, 3] > 0)
        )
        yy, xx = np.where(gold)
        x, y = int(xx.min() + left), int(yy.min() + top)
        width, height = int(xx.max() - xx.min() + 1), int(yy.max() - yy.min() + 1)
        bounds = font.getbbox(str(i))
        glyph = Image.new("L", (bounds[2] - bounds[0], bounds[3] - bounds[1]))
        ImageDraw.Draw(glyph).text(
            (-bounds[0], -bounds[1]), str(i), font=font, fill=255
        )
        glyph = glyph.crop(glyph.getbbox()).resize(
            (width * 4, height * 4), Image.Resampling.LANCZOS
        )
        patch = Image.new("RGBA", (96, 96))
        mask = Image.new("L", (96, 96))
        mask.paste(glyph, ((x - left) * 4, (y - top) * 4))
        outline = mask.filter(ImageFilter.MaxFilter(7))
        shadow = Image.new("L", (96, 96))
        shadow.paste(outline, (2, 2))
        bg = Image.new("RGBA", (96, 96), (20, 10, 3, 255))
        bg.putalpha(shadow)
        patch.alpha_composite(bg)
        ink = Image.new("RGBA", (96, 96), (246, 217, 57, 255))
        ink.putalpha(mask)
        patch.alpha_composite(ink)
        canvas.paste((0, 0, 0, 0), (left * 4, top * 4, right * 4, bottom * 4))
        canvas.alpha_composite(patch, (left * 4, top * 4))
        changed[top * 4 : bottom * 4, left * 4 : right * 4] = True
        alpha = np.array(patch)[:, :, 3]
        yy, xx = np.where(alpha > 0)
        checks.append(
            {
                "kind": "digit",
                "index": i,
                "native": [left, top, right, bottom],
                "visible": [
                    int(xx.min()),
                    int(yy.min()),
                    int(xx.max() + 1),
                    int(yy.max() + 1),
                ],
            }
        )
        if xx.min() <= 0 or yy.min() <= 0 or xx.max() >= 95 or yy.max() >= 95:
            raise ValueError(f"Digit outline exceeds native gutter: {checks[-1]}")
    result = np.array(canvas)
    if not np.array_equal(result[~changed], baseline[~changed]):
        raise ValueError("Stat vectors changed pixels outside their native rectangles")
    return result, checks
