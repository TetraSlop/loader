"""Enlarge the original 42 font atlases without replacing or guessing glyphs.

Only alpha coverage is filtered. Every cell, UV, RGB value and native text
metric retains its original meaning. Requires Pillow and NumPy, not an AI model.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, __version__ as pillow_version

from upscale_textures import contained

SCALE = 4
PITCH = 18
GRID = 14
FLOOR = 1
SHARPEN = 0.20
DECODED_SHA256 = "422e46ab2ad6fd02db570a2384424dd1540b197e1ccd7e3da2645ca2b40b1879"

# Printable ASCII 32..126: glyph index, post advance, pre offset, left/right
# pair classes. Derived from native table +236F28 and converter +185130;
# all 95 conversions were independently executed in Unicorn. No font outlines.
ASCII_METRICS = """
0 10 -5 0 0
9 10 -5 0 0
40 14 -6 0 0
83 13 -2 0 0
79 12 -3 0 0
82 13 -2 0 0
84 14 -2 0 0
38 4 1 0 0
41 12 -4 0 0
42 12 -4 0 0
85 12 -3 0 0
59 14 -1 0 0
3 7 -2 0 0
60 14 -1 0 0
4 7 -2 0 0
30 14 -1 0 0
203 13 -3 0 0
204 13 -3 0 0
205 13 -3 0 0
206 13 -3 0 0
207 13 -3 0 0
208 13 -3 0 0
209 13 -3 0 0
210 13 -3 0 0
211 13 -3 0 0
212 13 -3 0 0
6 10 -5 0 0
7 10 -5 0 0
66 14 -1 0 0
64 14 -1 0 0
67 14 -1 0 0
8 13 -2 0 0
86 14 -1 0 0
220 14 -1 -1 -1
221 13 -2 0 0
222 13 -2 0 0
223 13 -2 0 0
224 13 -2 0 0
225 13 -3 0 1
226 13 -2 0 0
227 13 -2 0 0
228 10 -5 0 0
229 12 -3 0 0
230 14 -2 0 0
231 13 -3 0 -1
232 14 -1 0 0
233 13 -2 0 0
234 13 -2 0 0
235 13 -2 0 0
236 13 -2 0 0
237 14 -2 0 0
238 13 -2 0 0
239 13 -2 1 1
240 13 -2 0 0
241 13 -2 1 1
242 15 0 1 1
243 13 -2 1 1
244 13 -2 1 1
245 13 -2 0 0
45 12 -4 0 0
31 14 -1 0 0
46 12 -4 0 0
15 13 -3 0 0
17 15 0 0 0
37 15 -10 0 0
252 12 -3 -1 -1
253 12 -3 0 -1
254 12 -3 -1 -1
255 12 -3 -1 0
256 12 -3 -1 -1
257 11 -4 0 0
258 12 -3 -1 -1
259 12 -3 0 -1
260 10 -5 0 0
261 10 -5 0 0
262 12 -3 0 -1
263 10 -5 0 0
264 15 0 -1 -1
265 12 -3 -1 -1
266 12 -3 -1 -1
267 12 -3 0 -1
268 12 -3 -1 0
269 10 -5 -1 -1
270 12 -3 -1 -1
271 11 -4 0 0
272 12 -3 -1 -1
273 12 -3 -1 -1
274 14 -1 -1 -1
275 12 -3 -1 -1
276 12 -3 -1 -1
277 12 -3 -1 -1
47 12 -4 0 0
34 10 -5 0 0
48 12 -4 0 0
32 14 -1 0 0
"""
METRICS = dict(zip(map(chr, range(32, 127)),
                   [tuple(map(int, row.split())) for row in ASCII_METRICS.splitlines() if row]))


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def cells():
    # The native 17px UV rectangle begins at (2+18*c, 1+18*r). Keep its
    # one-pixel left/top gutter in this disjoint 18px filtering partition.
    for row in range(GRID):
        for column in range(GRID):
            yield 1 + column * PITCH, row * PITCH


def enlarge_alpha(alpha: np.ndarray, sharpen: float = SHARPEN) -> np.ndarray:
    if alpha.shape != (256, 256) or alpha.min() != FLOOR:
        raise ValueError("Expected a 256x256 font atlas with alpha floor 1")
    output = np.repeat(np.repeat(alpha, SCALE, axis=0), SCALE, axis=1)
    for x, y in cells():
        coverage = (alpha[y:y + PITCH, x:x + PITCH].astype(np.float32) - FLOOR) / 254
        # Padding avoids the image resizer's edge extension at each cell's
        # boundary; no neighboring glyph enters the Lanczos filter support.
        padded = Image.fromarray(np.pad(coverage, 3))
        enlarged = np.asarray(padded.resize((96, 96), Image.Resampling.LANCZOS))
        q = np.clip(enlarged[12:84, 12:84], 0, 1)
        # A bounded, symmetric S curve. At strength .20 it changes coverage
        # by at most 4.89/255, keeping 0, .5 and 1 fixed. No dilation or shift.
        q = q + sharpen * q * (1 - q) * (2 * q - 1)
        result = np.rint(FLOOR + 254 * q).astype(np.uint8)
        output[y * SCALE:(y + PITCH) * SCALE, x * SCALE:(x + PITCH) * SCALE] = result
    return output


def cell_statistics(source: np.ndarray, enlarged: np.ndarray) -> dict:
    ratios, empty = [], 0
    for x, y in cells():
        before = source[y:y + PITCH, x:x + PITCH].astype(np.float64) - FLOOR
        after = enlarged[y * SCALE:(y + PITCH) * SCALE,
                         x * SCALE:(x + PITCH) * SCALE].astype(np.float64) - FLOOR
        if not before.any():
            if after.any():
                raise ValueError("An empty glyph cell gained coverage")
            empty += 1
        else:
            ratio = float(after.sum() / (before.sum() * SCALE * SCALE))
            if not 0.8 <= ratio <= 1.2:
                raise ValueError(f"Unexpected integrated glyph coverage drift: {ratio}")
            ratios.append(ratio)
    return {"empty_cells_preserved": empty,
            "coverage_ratio_range": [min(ratios), max(ratios)] if ratios else None}


def bilinear(alpha: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    x, y = np.clip(x, 0, alpha.shape[1] - 1), np.clip(y, 0, alpha.shape[0] - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, alpha.shape[1] - 1), np.minimum(y0 + 1, alpha.shape[0] - 1)
    dx, dy = x - x0, y - y0
    return ((1 - dy)[:, None] * ((1 - dx) * alpha[y0[:, None], x0] + dx * alpha[y0[:, None], x1])
            + dy[:, None] * ((1 - dx) * alpha[y1[:, None], x0] + dx * alpha[y1[:, None], x1]))


def draw_text(canvas: np.ndarray, atlases: dict, text: str, x: float, y: float,
              font_pixels: float, atlas_scale: int) -> list:
    """Preview native ASCII advances and UVs with a software bilinear sampler.

    Native quad width is requested font size (+1849EF), despite sampling a
    17px source region. This preview does not emulate the game's text effects.
    """
    placements, previous = [], 0
    for character in text:
        index, advance, offset, left, right = METRICS[character]
        x += (offset - (2 if previous * left < 0 else 0)) * font_pixels / 16
        atlas, slot = divmod(index, GRID * GRID)
        row, column = divmod(slot, GRID)
        source_x, source_y = 2 + PITCH * column, 1 + PITCH * row
        x0, x1 = max(0, int(np.ceil(x))), min(canvas.shape[1], int(np.ceil(x + font_pixels)))
        y0, y1 = max(0, int(np.ceil(y))), min(canvas.shape[0], int(np.ceil(y + font_pixels)))
        xs = ((np.arange(x0, x1) + .5 - x) / font_pixels * 17 + source_x) * atlas_scale - .5
        ys = ((np.arange(y0, y1) + .5 - y) / font_pixels * 17 + source_y) * atlas_scale - .5
        coverage = bilinear(atlases[atlas], xs, ys)[:, :, None] / 255
        target = canvas[y0:y1, x0:x1]
        target[:] = 255 * coverage + target * (1 - coverage)
        placements.append([character, round(x, 4), round(y, 4), font_pixels, atlas, slot])
        x += advance * font_pixels / 16
        previous = right
    return placements


def write_reviews(output: Path, originals: dict, enhanced: dict) -> dict:
    review = output / "review"
    review.mkdir()
    records = []
    samples = ["Tetra Master", "Welcome, Player 01!", "Win: 28  Loss: 7",
               "AaIiWw 0123456789", "AVATAR / Moogle", "!?:;., +-*/()[]"]
    for height, scale in ((1440, 3), (2160, 4.5)):
        font_pixels = 16 * scale
        width, row_height = round(260 * scale), round(23 * scale)
        header = 52
        canvas = np.full((header + row_height * len(samples) + 20, width * 2, 3), 30, dtype=np.float64)
        placements = []
        for row, sample in enumerate(samples):
            y = header + row * row_height
            before = draw_text(canvas, originals, sample, 12, y, font_pixels, 1)
            after = draw_text(canvas, enhanced, sample, width + 12, y, font_pixels, SCALE)
            # Apart from the comparison column offset, layout must be identical.
            if [(g[0], round(g[1] + width, 4), *g[2:]) for g in before] != [tuple(g) for g in after]:
                raise ValueError("Preview changed glyph layout")
            placements.append({"text": sample, "original": before, "enhanced": after})
        image = Image.fromarray(np.rint(canvas).astype(np.uint8))
        draw = ImageDraw.Draw(image)
        draw.text((12, 8), f"Original atlas, bilinear | {height}p display / {font_pixels:g}px quad", fill="white")
        draw.text((width + 12, 8), "4x isolated Lanczos + conservative coverage | identical metrics", fill="white")
        draw.text((12, 27), "Software sampling preview; 4:3 viewport, 16-unit native font; no text effects", fill=(180, 180, 180))
        destination = review / f"font-text-{height}p.png"
        image.save(destination)
        records.append({"file": destination.name, "sha256": digest(destination.read_bytes()),
                        "display_height": height, "native_viewport_scale": scale,
                        "font_pixels": font_pixels, "placements": placements})
    # Exact 1:1 pixel comparison of a complete small atlas region, including
    # punctuation and Japanese glyphs that the ASCII preview does not map.
    original = Image.fromarray(originals[0].astype(np.uint8)).crop((0, 0, 256, 72))
    original = original.resize((1024, 288), Image.Resampling.BILINEAR)
    filtered = Image.fromarray(enhanced[0].astype(np.uint8)).crop((0, 0, 1024, 288))
    sheet = Image.new("RGB", (1024, 636), (30, 30, 30))
    for alpha, y, label in ((original, 28, "Original bilinear 4x"),
                            (filtered, 344, "Cell-isolated 4x: no character substitutions")):
        white = Image.new("RGB", alpha.size, "white")
        sheet.paste(white, (0, y), alpha)
        ImageDraw.Draw(sheet).text((8, y - 22), label, fill="white")
    sheet.save(review / "font-atlas-detail.png")
    result = {"method": "software bilinear sampling with native printable ASCII mapping/metrics",
              "decoded_image_sha256": DECODED_SHA256,
              "metrics_sha256": digest(ASCII_METRICS.strip().encode()), "previews": records}
    (review / "review.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def self_check() -> None:
    if len(METRICS) != 95:
        raise ValueError("Incomplete printable ASCII metrics")
    # A mark at a cell boundary must not enter its neighbor or external gutter.
    source = np.ones((256, 256), dtype=np.uint8)
    source[8:12, 17:19] = 255
    enlarged = enlarge_alpha(source)
    if np.any(enlarged[:, 19 * SCALE:] != FLOOR) or np.any(enlarged[:, :SCALE] != FLOOR):
        raise ValueError("Filtering crossed a glyph-cell boundary")
    cell_statistics(source, enlarged)
    if np.any(enlarge_alpha(np.ones((256, 256), dtype=np.uint8)) != FLOOR):
        raise ValueError("Blank atlas changed")
    q = np.linspace(0, 1, 1001)
    adjusted = q + SHARPEN * q * (1 - q) * (2 * q - 1)
    if np.any(np.diff(adjusted) < 0) or np.max(np.abs(adjusted - q)) * 254 > 4.9:
        raise ValueError("Coverage adjustment is not conservative and monotonic")


def build(args) -> dict:
    self_check()
    root = args.lab.resolve()
    output = contained(root, args.output.as_posix())
    if output == root or output.is_relative_to(root / "original") or output.exists():
        raise ValueError("Choose a new output directory separate from original exports")
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    sources = {record["id"]: record for record in manifest["textures"] if record["font_atlas"]}
    ids = [f"gW000/0090/font_{index:02d}" for index in range(42)]
    if set(sources) != set(ids):
        raise ValueError("Expected exactly font_00 through font_41")
    images, records, originals, enhanced = [], [], {}, {}
    for index, texture_id in enumerate(ids):
        record = sources[texture_id]
        path = contained(root, record["file"])
        if digest(path.read_bytes()) != record["sha256"]:
            raise ValueError(f"Original hash mismatch: {texture_id}")
        with Image.open(path) as image:
            rgba = np.asarray(image.convert("RGBA"))
        if rgba.shape != (256, 256, 4) or np.any(rgba[:, :, :3] != 255):
            raise ValueError(f"Unexpected font RGB or dimensions: {texture_id}")
        alpha = enlarge_alpha(rgba[:, :, 3])
        statistics = cell_statistics(rgba[:, :, 3], alpha)
        final = np.full((1024, 1024, 4), 255, dtype=np.uint8)
        final[:, :, 3] = alpha
        images.append((texture_id, Image.fromarray(final)))
        records.append({"id": texture_id, "source_sha256": record["sha256"],
                        "logical_size": [256, 256], "physical_size": [1024, 1024],
                        "file": texture_id + ".png", "validation": statistics})
        if index < 2:  # Printable ASCII previews only use atlases 00 and 01.
            originals[index] = rgba[:, :, 3].astype(np.float64)
            enhanced[index] = alpha.astype(np.float64)
    output.mkdir(parents=True)
    for (texture_id, image), record in zip(images, records):
        path = contained(output, record["file"])
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path)
        record["sha256"] = digest(path.read_bytes())
        print(f"{texture_id}: 256x256 -> 1024x1024; {record['sha256']}")
    pack = {"version": 1, "generator": "tmloader/build_font_pack.py",
            "method": "per-cell Lanczos with bounded alpha coverage S curve",
            "scale": SCALE, "coverage_sharpen": SHARPEN, "alpha_floor": FLOOR,
            "layout": {"grid": [14, 14], "cell_pitch": [18, 18], "sample_offset": [2, 1],
                       "sample_extent": [17, 17], "logical_atlas": [256, 256]},
            "decoded_image_sha256": DECODED_SHA256,
            "dependencies": {"Pillow": pillow_version, "numpy": np.__version__},
            "textures": records}
    (output / "pack.json").write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    write_reviews(output, originals, enhanced)
    print(f"Prepared all 42 font atlases and 1440p/2160p text previews in {output}")
    return pack


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--output", type=Path, default=Path("variants/fonts-4x"),
                        help="New output directory relative to --lab; existing outputs are preserved")
    parser.add_argument("--self-check", action="store_true", help="Check cell isolation and coverage without assets")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        print("PASS: cell isolation, blank coverage, monotonic sharpening and 95 ASCII metrics")
    else:
        build(args)
