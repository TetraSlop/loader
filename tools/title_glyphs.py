"""Redraw audited title/PlayOnline bitmap cells without changing native layout.

These are deliberate vector substitutions, not recovered original typefaces.
The native TM code image (SHA256 422e46ab2ad6fd02db570a2384424dd1540b197e1ccd7e3da2645ca2b40b1879)
provides the rectangles below. PNG artwork companions already cover these
atlases; none of these cells uses TM's separate font-atlas renderer.
"""

from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# +148100..1488D5 builds Ver.1.06.011 using these source cells. The other
# beta/version-suffix cells remain untouched. Copyright is +14AC41/+14ACAF.
VERSION_WIDTHS = (10, 8, 9, 9, 10, 9, 9, 9, 9, 10)
TITLE_LABELS = (
    ("©2000–2004 SQUARE ENIX", (0, 0, 200, 19)),
    ("CO., LTD. All Rights Reserved.", (0, 24, 195, 43)),
    ("Ver.", (0, 48, 24, 64)),
    (".", (24, 48, 28, 64)),
    *(
        (str(i), (32 + 16 * i, 48, 32 + 16 * i + w, 64))
        for i, w in enumerate(VERSION_WIDTHS)
    ),
)

# +164Dxx/+1653xx call +18AE20(x,y,w,h), which samples (w+1,h+1)
# texels on a (w,h) quad. Keep that extra texel and native state variants.
POL_LABELS = (
    ("PlayOnline", (0, 120, 75, 139)),
    ("PlayOnline", (80, 120, 155, 139)),
    ("PlayOnline", (0, 144, 75, 163)),
    ("Friend List", (40, 24, 140, 43)),
    ("Friend List", (40, 48, 140, 67)),
    ("Friend List", (40, 96, 140, 115)),
)
KEY_CELLS = (
    ("F1", (80, 144, 112, 162)),
    ("F2", (112, 144, 144, 162)),
    ("F3", (144, 144, 176, 162)),
)

# +165702/+1658E1: friend2 chat and mail; one extra sampled texel is
# retained from baseline instead of overwriting a neighbouring sprite.
STATUS_CELLS = (("chat", (0, 48, 32, 80)), ("mail", (32, 48, 72, 80)))
# +165795/+16596A: friend3 counts 0..9; ".." at x216 is unchanged.
COUNTER_LABELS = tuple((str(i), (56 + 16 * i, 0, 70 + 16 * i, 17)) for i in range(10))


def _inputs(source, baseline, size):
    source, baseline = np.asarray(source), np.asarray(baseline)
    if source.dtype != np.uint8 or source.shape != (size[1], size[0], 4):
        raise ValueError(f"Expected source RGBA uint8 atlas {size}")
    if baseline.dtype != np.uint8 or baseline.ndim != 3 or baseline.shape[2] != 4:
        raise ValueError("Expected baseline RGBA uint8 atlas")
    scale = baseline.shape[1] // size[0]
    if scale < 2 or baseline.shape[:2] != (size[1] * scale, size[0] * scale):
        raise ValueError("Baseline must have one integer enlargement scale >=2")
    return source, baseline, scale


def _ink_bounds(source, rect, *, bright=True):
    left, top, right, bottom = rect
    crop = source[top:bottom, left:right]
    mask = crop[:, :, 3] > 128
    if bright and mask.any():
        luminance = crop[:, :, :3].astype(float) @ np.array((0.2126, 0.7152, 0.0722))
        mask &= luminance > luminance[mask].max() * 0.82
    yy, xx = np.where(mask)
    if not len(xx):
        raise ValueError(f"No original ink in audited cell {rect}")
    return (
        int(xx.min()) + left,
        int(yy.min()) + top,
        int(xx.max()) + left + 1,
        int(yy.max()) + top + 1,
    )


def _glyph(font, text, size):
    bounds = font.getbbox(text)
    image = Image.new("L", (bounds[2] - bounds[0], bounds[3] - bounds[1]))
    ImageDraw.Draw(image).text((-bounds[0], -bounds[1]), text, font=font, fill=255)
    return image.crop(image.getbbox()).resize(size, Image.Resampling.LANCZOS)


def _paste_text(
    canvas,
    source,
    rect,
    label,
    font,
    scale,
    *,
    palette=None,
    stroke=0.5,
    fill_rect=True,
    highlighted=False,
):
    left, top, right, bottom = rect
    x0, y0, x1, y1 = _ink_bounds(source, rect)
    # Derive color from the original ink, including disabled/selected states.
    crop = source[top:bottom, left:right]
    lum = crop[:, :, :3].astype(float) @ np.array((0.2126, 0.7152, 0.0722))
    if palette is None:
        maximum = np.max(lum[crop[:, :, 3] > 128])
        palette = tuple(
            np.median(
                crop[(lum > maximum * 0.94) & (crop[:, :, 3] > 128), :3], axis=0
            ).astype(int)
        )
    patch = Image.new("RGBA", ((right - left) * scale, (bottom - top) * scale))
    mask = Image.new("L", patch.size)
    mask.paste(
        _glyph(font, label, ((x1 - x0) * scale, (y1 - y0) * scale)),
        ((x0 - left) * scale, (y0 - top) * scale),
    )
    radius = max(1, round(stroke * scale))
    border = mask.filter(ImageFilter.MaxFilter(2 * radius + 1))
    if highlighted:
        glow = Image.new("RGBA", patch.size, (255, 142, 32, 255))
        glow.putalpha(border.filter(ImageFilter.GaussianBlur(scale * 0.4)))
        patch.alpha_composite(glow)
    shadow = Image.new(
        "RGBA", patch.size, (211, 96, 12, 255) if highlighted else (10, 10, 13, 255)
    )
    shadow.putalpha(border)
    patch.alpha_composite(shadow)
    ink = Image.new("RGBA", patch.size, (*palette, 255))
    ink.putalpha(mask)
    patch.alpha_composite(ink)
    box = (left * scale, top * scale, right * scale, bottom * scale)
    if fill_rect:
        canvas.paste((0, 0, 0, 0), box)
    canvas.alpha_composite(patch, box[:2])
    return {"label": label, "rect": list(rect), "ink": [x0, y0, x1, y1]}


def _finish(canvas, baseline, scale, checks, method):
    result = np.array(canvas)
    allowed = np.zeros(baseline.shape[:2], bool)
    for check in checks:
        l, t, r, b = check["rect"]
        allowed[t * scale : b * scale, l * scale : r * scale] = True
    if not np.array_equal(result[~allowed], baseline[~allowed]):
        raise ValueError("Reconstruction changed unaudited pixels")
    return result, {
        "method": method,
        "scale": scale,
        "regions": checks,
        "outside_regions_unchanged": True,
    }


def refine_title_atlas(source, baseline, font_file):
    """Retype copyright and live version cells, preserving beta/suffix artwork."""
    source, baseline, scale = _inputs(source, baseline, (256, 96))
    canvas = Image.fromarray(baseline)
    font = ImageFont.truetype(str(font_file), 256)
    checks = [
        _paste_text(
            canvas,
            source,
            rect,
            label,
            font,
            scale,
            palette=(255, 255, 255),
            stroke=0.3,
        )
        for label, rect in TITLE_LABELS
    ]
    return _finish(
        canvas,
        baseline,
        scale,
        checks,
        "vector copyright/version; original ink extents",
    )


def refine_pol_atlas(source, baseline, font_file):
    """Retype toolbar state variants and F1/F2/F3 keycaps in friend4."""
    source, baseline, scale = _inputs(source, baseline, (200, 168))
    canvas = Image.fromarray(baseline)
    font = ImageFont.truetype(str(font_file), 256)
    checks = [
        _paste_text(
            canvas,
            source,
            rect,
            label,
            font,
            scale,
            stroke=0.7,
            highlighted=(rect[1] == 48 or rect[:2] == (80, 120)),
        )
        for label, rect in POL_LABELS
    ]
    for label, rect in KEY_CELLS:
        l, t, r, b = rect
        bounds = _ink_bounds(source, rect, bright=False)
        x0, y0, x1, y1 = bounds
        patch = Image.new("RGBA", ((r - l) * scale, (b - t) * scale))
        draw = ImageDraw.Draw(patch)
        box = (
            (x0 - l) * scale,
            (y0 - t) * scale,
            (x1 - l) * scale - 1,
            (y1 - t) * scale - 1,
        )
        draw.rounded_rectangle(
            box,
            radius=scale,
            fill=(214, 194, 158, 255),
            outline=(73, 59, 45, 255),
            width=scale,
        )
        # Raised beige edge and a smaller legend retain the original key feel.
        inset = scale
        draw.line(
            (box[0] + inset, box[1] + inset, box[2] - inset, box[1] + inset),
            fill=(247, 232, 196, 255),
            width=max(1, scale // 2),
        )
        draw.line(
            (box[0] + inset, box[1] + inset, box[0] + inset, box[3] - inset),
            fill=(238, 220, 183, 255),
            width=max(1, scale // 2),
        )
        draw.line(
            (box[0] + inset, box[3] - inset, box[2] - inset, box[3] - inset),
            fill=(136, 114, 83, 255),
            width=scale,
        )
        width, height = (x1 - x0 - 8) * scale, (y1 - y0 - 6) * scale
        if min(width, height) <= 0:
            raise ValueError(f"Keycap has no interior: {label}")
        mask = _glyph(font, label, (width, height))
        ink = Image.new("RGBA", mask.size, (28, 31, 29, 255))
        ink.putalpha(mask)
        patch.alpha_composite(ink, ((x0 - l + 4) * scale, (y0 - t + 3) * scale))
        canvas.paste((0, 0, 0, 0), (l * scale, t * scale, r * scale, b * scale))
        canvas.alpha_composite(patch, (l * scale, t * scale))
        checks.append({"label": label, "rect": list(rect), "ink": list(bounds)})
    return _finish(
        canvas,
        baseline,
        scale,
        checks,
        "vector toolbar labels and keycaps; original layout",
    )


def refine_status_icons_atlas(source, baseline):
    """Clean chat bubble/envelope silhouettes, retaining native 32/40px cells."""
    source, baseline, scale = _inputs(source, baseline, (256, 96))
    canvas = Image.fromarray(baseline)
    checks = []
    for label, rect in STATUS_CELLS:
        l, t, r, b = rect
        bounds = _ink_bounds(source, rect, bright=False)
        x0, y0, x1, y1 = bounds
        # Construct smooth shapes at a finer scale, then rasterize once.
        ss = scale * 4
        patch = Image.new("RGBA", ((x1 - x0) * ss, (y1 - y0) * ss))
        d = ImageDraw.Draw(patch)
        w, h = patch.size

        def xy(u, v, w=w, h=h):
            return (round(u * (w - 1)), round(v * (h - 1)))

        edge, fill = (43, 90, 122, 255), (225, 243, 250, 255)
        line = max(1, round(1.3 * ss))
        if label == "chat":
            d.rounded_rectangle(
                (*xy(0.06, 0.04), *xy(0.94, 0.77)),
                radius=4 * ss,
                fill=fill,
                outline=edge,
                width=line,
            )
            tail = [xy(0.50, 0.66), xy(0.54, 0.98), xy(0.79, 0.68)]
            d.polygon(tail, fill=fill)
            d.line(tail[:2] + tail[2:], fill=edge, width=line, joint="curve")
            for u, v in ((0.29, 0.36), (0.55, 0.32), (0.79, 0.28)):
                d.ellipse((*xy(u - 0.07, v - 0.06), *xy(u + 0.07, v + 0.08)), fill=edge)
        else:
            points = [xy(0.27, 0.03), xy(0.98, 0.27), xy(0.77, 0.97), xy(0.02, 0.71)]
            d.polygon(points, fill=fill)
            d.line(points + [points[0]], fill=edge, width=line, joint="curve")
            middle = xy(0.47, 0.51)
            d.line([points[0], middle, points[1]], fill=edge, width=line, joint="curve")
            # Lower folded paper seams remain lighter than the flap.
            d.line([points[3], xy(0.38, 0.49)], fill=(117, 161, 190, 255), width=ss)
            d.line([points[2], xy(0.57, 0.48)], fill=(117, 161, 190, 255), width=ss)
        # Retain the original white/ice-blue badge treatment with restrained
        # shading and a blue halo, contained within the original ink bounds.
        pixels = np.array(patch)
        inside = np.all(pixels[:, :, :3] == fill[:3], axis=2) & (pixels[:, :, 3] > 0)
        gradient = (
            np.linspace((251, 254, 255), (162, 204, 231), h).round().astype(np.uint8)
        )
        for row in range(h):
            pixels[row, inside[row], :3] = gradient[row]
        patch = Image.fromarray(pixels)
        d = ImageDraw.Draw(patch)
        if label == "chat":
            highlight = [xy(0.23, 0.105), xy(0.79, 0.105)]
        else:
            highlight = [xy(0.29, 0.10), xy(0.91, 0.30)]
        d.line(highlight, fill=(255, 255, 255, 220), width=max(1, round(ss * 0.45)))
        halo = Image.new("RGBA", patch.size, (74, 136, 182, 255))
        halo.putalpha(
            patch.getchannel("A")
            .filter(ImageFilter.GaussianBlur(ss * 0.7))
            .point(lambda a: round(a * 0.6))
        )
        halo.alpha_composite(patch)
        patch = halo.resize(
            ((x1 - x0) * scale, (y1 - y0) * scale), Image.Resampling.LANCZOS
        )
        canvas.paste((0, 0, 0, 0), (l * scale, t * scale, r * scale, b * scale))
        canvas.alpha_composite(patch, (x0 * scale, y0 * scale))
        checks.append({"label": label, "rect": list(rect), "ink": list(bounds)})
    return _finish(
        canvas,
        baseline,
        scale,
        checks,
        "vector chat/mail substitutions; original cell extents",
    )


def refine_status_counter_atlas(source, baseline, font_file):
    """Retype friend3 decimal counters; preserve the unknown-count '..' cell."""
    source, baseline, scale = _inputs(source, baseline, (256, 48))
    canvas = Image.fromarray(baseline)
    font = ImageFont.truetype(str(font_file), 256)
    checks = [
        _paste_text(
            canvas,
            source,
            rect,
            label,
            font,
            scale,
            palette=(230, 245, 255),
            stroke=0.8,
        )
        for label, rect in COUNTER_LABELS
    ]
    return _finish(
        canvas, baseline, scale, checks, "vector status counters; original ink extents"
    )
