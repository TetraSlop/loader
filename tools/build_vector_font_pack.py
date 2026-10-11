"""Replace the 95 printable ASCII font cells with MS UI Gothic vector glyphs fitted to the native ink boxes.

The original typeface is unidentified. MS UI Gothic had the lowest normalized alpha error of the installed
Windows fonts. Unmapped cells are copied unchanged from the conservative pack
built by build_font_pack.py.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from build_font_pack import FLOOR, GRID, METRICS, PITCH, SCALE, draw_text
from title_glyphs import render_text
from upscale_textures import contained, digest

# MS UI Gothic is face 1 of the MS Gothic collection.
FONT_INDEX = 1


def native_character(index: int) -> str:
    """Map a native glyph index to its character; indices follow JIS X 0208 order starting at 0x2121."""
    row, column = divmod(index + 0xC3F - 0x21, 94)
    return (b"\x1b$B" + bytes((row, column + 0x21)) + b"\x1b(B").decode("iso2022_jp")


def check_mapping() -> None:
    expected = {0: "\u3000", 9: "\uff01", 85: "\uff0a", 220: "\uff21", 277: "\uff5a"}
    if any(native_character(index) != character for index, character in expected.items()):
        raise ValueError("Native JIS mapping self-check failed")
    if len({record[0] for record in METRICS.values()}) != 95:
        raise ValueError("Verified ASCII cells unexpectedly alias")


def source_cells(atlases: dict) -> dict:
    result = {}
    for character, (index, *_) in METRICS.items():
        atlas, slot = divmod(index, GRID * GRID)
        row, column = divmod(slot, GRID)
        x, y = 1 + column * PITCH, row * PITCH
        alpha = atlases[atlas][y : y + PITCH, x : x + PITCH]
        mask = Image.fromarray(alpha - FLOOR)
        result[character] = {"index": index, "atlas": atlas, "xy": (x, y), "alpha": alpha, "ink_box": mask.getbbox()}
    return result


def fit_glyph(font: ImageFont.FreeTypeFont, source: dict) -> np.ndarray:
    """Render the cell's native JIS character into its original ink box at 4x."""
    output = np.full((PITCH * SCALE, PITCH * SCALE), FLOOR, dtype=np.uint8)
    if source["ink_box"] is None:
        return output
    x0, y0, x1, y1 = source["ink_box"]
    image = render_text(font, native_character(source["index"]), ((x1 - x0) * SCALE, (y1 - y0) * SCALE))
    coverage = np.asarray(image).astype(np.float32) / 255
    output[y0 * SCALE : y1 * SCALE, x0 * SCALE : x1 * SCALE] = np.rint(FLOOR + 254 * coverage).astype(np.uint8)
    return output


def replace_cells(base: dict, sources: dict, glyphs: dict) -> dict:
    result = {index: alpha.copy() for index, alpha in base.items()}
    changed = {index: np.zeros(alpha.shape, dtype=bool) for index, alpha in base.items()}
    for character, source in sources.items():
        x, y = (n * SCALE for n in source["xy"])
        atlas = source["atlas"]
        result[atlas][y : y + PITCH * SCALE, x : x + PITCH * SCALE] = glyphs[character]
        changed[atlas][y : y + PITCH * SCALE, x : x + PITCH * SCALE] = True
    for index in base:
        if not np.array_equal(result[index][~changed[index]], base[index][~changed[index]]):
            raise ValueError("An unmapped font cell changed")
    return result


def reviews(output: Path, original: dict, conservative: dict, vector: dict, family: str) -> None:
    review = output / "review"
    review.mkdir()
    width, header, row_height, font_pixels = 930, 60, 104, 72
    samples = (
        "Tetra Master",
        "Welcome, Player 01!",
        "Win: 28  Loss: 7",
        "AaIiWw 0123456789",
        "AVATAR / Moogle",
        "!?:;., +-*/()[]",
    )
    canvas = np.full((header + row_height * len(samples), width * 3, 3), 30, dtype=float)
    placements = []
    columns = (
        (original, 1, "Original bitmap / bilinear"),
        (conservative, 4, "Conservative 4x glyph filtering"),
        (vector, 4, family + " vector Latin / native ink boxes"),
    )
    for row, sample in enumerate(samples):
        positions = []
        for column, (atlases, scale, _) in enumerate(columns):
            placed = draw_text(
                canvas, atlases, sample, 12 + column * width, header + row * row_height, font_pixels, scale
            )
            positions.append([(g[0], round(g[1] - column * width, 4), *g[2:]) for g in placed])
        if not positions[0] == positions[1] == positions[2]:
            raise ValueError("A preview changed the native layout")
        placements.append({"text": sample, "identical_native_layout": positions[0]})
    image = Image.fromarray(np.rint(canvas).astype(np.uint8))
    draw = ImageDraw.Draw(image)
    for column, (_, _, title) in enumerate(columns):
        draw.text((12 + column * width, 9), title, fill="white")
        draw.text((12 + column * width, 29), "2160p / 4:3 viewport / 72px native glyph quad", fill=(180, 180, 180))
    image.save(review / "font-latin-2160p.png")
    (review / "review.json").write_text(
        json.dumps(
            {
                "display_height": 2160,
                "font_pixels": font_pixels,
                "samples": placements,
                "limitation": "Software preview omits native text effects.",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def build(args: argparse.Namespace) -> dict:
    check_mapping()
    lab, base = args.lab.resolve(), args.base.resolve()
    output = contained(lab, args.output.as_posix())
    if output.exists() or output == lab or output.is_relative_to(lab / "original"):
        raise ValueError("Choose a new output directory outside original exports")
    original_manifest = json.loads((lab / "manifest.json").read_text(encoding="utf-8"))
    original_records = {entry["id"]: entry for entry in original_manifest["textures"]}
    manifest_bytes = (base / "pack.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if len(manifest["textures"]) != 42:
        raise ValueError("Expected the complete 42-atlas conservative pack")
    data, originals, conservative = {}, {}, {}
    for index in range(42):
        texture_id = f"gW000/0090/font_{index:02d}"
        entry = next(record for record in manifest["textures"] if record["id"] == texture_id)
        original = original_records[texture_id]
        path = contained(base, entry["file"])
        png = path.read_bytes()
        if digest(png) != entry["sha256"] or entry["source_sha256"] != original["sha256"]:
            raise ValueError(f"Conservative pack hash mismatch: {texture_id}")
        data[index] = (entry, png)
        if index < 2:
            original_path = contained(lab, original["file"])
            if digest(original_path.read_bytes()) != original["sha256"]:
                raise ValueError(f"Original hash mismatch: {texture_id}")
            originals[index] = np.asarray(Image.open(original_path).convert("RGBA"))[:, :, 3]
            conservative[index] = np.asarray(Image.open(path).convert("RGBA"))[:, :, 3]
    sources = source_cells(originals)
    font = ImageFont.truetype(str(args.font), 256, index=FONT_INDEX)
    family, style = font.getname()
    vector = replace_cells(conservative, sources, {c: fit_glyph(font, source) for c, source in sources.items()})
    output.mkdir(parents=True)
    records = []
    for index, (entry, png) in data.items():
        target = contained(output, entry["file"])
        target.parent.mkdir(parents=True, exist_ok=True)
        if index < 2:
            rgba = np.full((1024, 1024, 4), 255, dtype=np.uint8)
            rgba[:, :, 3] = vector[index]
            Image.fromarray(rgba).save(target)
        else:
            target.write_bytes(png)
        record = {key: value for key, value in entry.items() if key != "validation"}
        record["sha256"] = digest(target.read_bytes())
        record["method"] = (
            "verified Latin vector cells plus conservative unmapped cells"
            if index < 2
            else "conservative pack, byte-identical"
        )
        records.append(record)
    pack = {
        "version": 1,
        "generator": "tmloader/build_vector_font_pack.py",
        "status": "deliberate Latin typeface replacement; original face not identified",
        "base_pack_sha256": digest(manifest_bytes),
        "font": {
            "family": family,
            "style": style,
            "file": args.font.name,
            "file_sha256": digest(args.font.read_bytes()),
            "collection_index": FONT_INDEX,
            "character_input": "native-jis",
        },
        "rasterization": "256px local vector rendering, ink crop fitted into each original glyph ink box at 4x",
        "mapped_cells": 95,
        "empty_mapped_cells": 1,
        "unmapped_cells": "all preserved exactly from conservative pack",
        "textures": records,
    }
    (output / "pack.json").write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    reviews(output, originals, conservative, vector, family)
    print(f"Rendered {family} {style}; saved {output}")
    return pack


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--base", type=Path, default=Path("out/texture-lab/variants/fonts-4x"))
    parser.add_argument("--output", type=Path, default=Path("variants/fonts-vector-4x-v2"))
    parser.add_argument("--font", type=Path, default=Path("C:/Windows/Fonts/msgothic.ttc"))
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
