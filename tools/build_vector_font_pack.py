"""Compare local vector fonts and replace only the 95 verified Latin cells.

This is an explicit alternative typeface, not recovery of the original font.
No font binaries or game images are distributed by this tool. Unmapped cells
are copied unchanged from the conservative font pack.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from build_font_pack import FLOOR, GRID, METRICS, PITCH, SCALE, digest, draw_text
from upscale_textures import contained

FONT_FILES = ("arial.ttf", "arialbd.ttf", "tahoma.ttf", "tahomabd.ttf",
              "verdana.ttf", "verdanab.ttf", "micross.ttf", "msgothic.ttc",
              "segoeui.ttf", "YuGothR.ttc")


def native_character(index: int) -> str:
    row, column = divmod(index + 0xC3F - 0x21, 94)
    return (b"\x1b$B" + bytes((row, column + 0x21)) + b"\x1b(B").decode("iso2022_jp")


def check_mapping() -> None:
    # Include punctuation past the first JIS row boundary, not just letters.
    expected = {0: "\u3000", 9: "\uff01", 85: "\uff0a", 220: "\uff21", 277: "\uff5a"}
    if any(native_character(index) != character for index, character in expected.items()):
        raise ValueError("Native JIS mapping self-check failed")
    if len({record[0] for record in METRICS.values()}) != 95:
        raise ValueError("Verified ASCII cells unexpectedly alias")


def glyph_image(font, character: str) -> Image.Image:
    left, top, right, bottom = font.getbbox(character)
    image = Image.new("L", (max(1, right - left), max(1, bottom - top)))
    ImageDraw.Draw(image).text((-left, -top), character, font=font, fill=255)
    box = image.getbbox()
    return image.crop(box) if box else Image.new("L", (1, 1))


def source_cells(atlases: dict) -> dict:
    result = {}
    for character, (index, *_) in METRICS.items():
        atlas, slot = divmod(index, GRID * GRID)
        row, column = divmod(slot, GRID)
        x, y = 1 + column * PITCH, row * PITCH
        alpha = atlases[atlas][y:y + PITCH, x:x + PITCH]
        mask = Image.fromarray(alpha - FLOOR)
        result[character] = {"index": index, "atlas": atlas, "xy": (x, y),
                             "alpha": alpha, "ink_box": mask.getbbox()}
    return result


def fit_glyph(font, character: str, source: dict, missing: Image.Image) -> np.ndarray:
    output = np.full((PITCH * SCALE, PITCH * SCALE), FLOOR, dtype=np.uint8)
    if source["ink_box"] is None:
        return output
    image = glyph_image(font, character)
    if image.size == missing.size and image.tobytes() == missing.tobytes():
        raise ValueError(f"Font lacks U+{ord(character):04X}")
    x0, y0, x1, y1 = source["ink_box"]
    image = image.resize(((x1 - x0) * SCALE, (y1 - y0) * SCALE), Image.Resampling.LANCZOS)
    coverage = np.asarray(image).astype(np.float32) / 255
    output[y0 * SCALE:y1 * SCALE, x0 * SCALE:x1 * SCALE] = np.rint(FLOOR + 254 * coverage).astype(np.uint8)
    return output


def candidates(fonts: Path, sources: dict) -> list:
    results = []
    for filename in FONT_FILES:
        path = fonts / filename
        if not path.is_file():
            continue
        for index in range(5 if path.suffix.lower() == ".ttc" else 1):
            try:
                font = ImageFont.truetype(str(path), 256, index=index)
            except OSError:
                break
            missing = glyph_image(font, "\U0010ffff")
            for encoding in ("ascii", "native-jis"):
                cells, scores = {}, []
                try:
                    for character, source in sources.items():
                        rendered = native_character(source["index"]) if encoding == "native-jis" else character
                        result = fit_glyph(font, rendered, source, missing)
                        cells[character] = result
                        if character.isascii() and character.isalnum():
                            # Box-average the 4x vector result onto the original
                            # pixel grid, then compare normalized alpha shapes.
                            sampled = result.reshape(PITCH, SCALE, PITCH, SCALE).mean(axis=(1, 3))
                            before = (source["alpha"].astype(float) - FLOOR) / 254
                            after = (sampled - FLOOR) / 254
                            scores.append(float(np.mean((before - after) ** 2) / np.mean(before ** 2)))
                except ValueError:
                    continue
                record = {"family": font.getname()[0], "style": font.getname()[1],
                          "file": filename, "file_sha256": digest(path.read_bytes()),
                          "collection_index": index, "character_input": encoding,
                          "normalized_alpha_mse": float(np.mean(scores))}
                results.append({"metadata": record, "cells": cells})
    return sorted(results, key=lambda candidate: candidate["metadata"]["normalized_alpha_mse"])


def replace_cells(base: dict, sources: dict, glyphs: dict) -> dict:
    result = {index: alpha.copy() for index, alpha in base.items()}
    changed = {index: np.zeros(alpha.shape, dtype=bool) for index, alpha in base.items()}
    for character, source in sources.items():
        x, y = (n * SCALE for n in source["xy"])
        atlas = source["atlas"]
        result[atlas][y:y + PITCH * SCALE, x:x + PITCH * SCALE] = glyphs[character]
        changed[atlas][y:y + PITCH * SCALE, x:x + PITCH * SCALE] = True
    for index in base:
        if not np.array_equal(result[index][~changed[index]], base[index][~changed[index]]):
            raise ValueError("An unmapped font cell changed")
    return result


def reviews(output: Path, original: dict, conservative: dict, vector: dict,
            sources: dict, ranked: list) -> None:
    review = output / "review"
    review.mkdir()
    width, header, row_height, font_pixels = 930, 60, 104, 72
    samples = ("Tetra Master", "Welcome, Player 01!", "Win: 28  Loss: 7",
               "AaIiWw 0123456789", "AVATAR / Moogle", "!?:;., +-*/()[]")
    canvas = np.full((header + row_height * len(samples), width * 3, 3), 30, dtype=float)
    placements = []
    columns = ((original, 1, "Original bitmap / bilinear"),
               (conservative, 4, "Conservative 4x glyph filtering"),
               (vector, 4, ranked[0]["metadata"]["family"] + " vector Latin / native ink boxes"))
    for row, sample in enumerate(samples):
        positions = []
        for column, (atlases, scale, _) in enumerate(columns):
            placed = draw_text(canvas, atlases, sample, 12 + column * width,
                               header + row * row_height, font_pixels, scale)
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
    # Every available family is represented by its best encoding/style result.
    families = {}
    for candidate in ranked:
        families.setdefault(candidate["metadata"]["family"], candidate)
    gallery = np.full((len(families) * 180, 1420, 3), 30, dtype=float)
    labels = []
    for row, candidate in enumerate(families.values()):
        atlas = replace_cells(conservative, sources, candidate["cells"])
        draw_text(gallery, atlas, "Tetra Master  AaIiWw 0123456789", 12, row * 180 + 30, 64, 4)
        draw_text(gallery, atlas, "Welcome, Player! AVATAR / Moogle", 12, row * 180 + 96, 64, 4)
        data = candidate["metadata"]
        labels.append((row * 180 + 8, f"{data['family']} {data['style']} / {data['character_input']} / shape error {data['normalized_alpha_mse']:.4f}"))
    image = Image.fromarray(np.rint(gallery).astype(np.uint8))
    draw = ImageDraw.Draw(image)
    for y, label in labels:
        draw.text((12, y), label, fill="white")
    image.save(review / "installed-font-candidates.png")
    (review / "review.json").write_text(json.dumps({"display_height": 2160,
        "font_pixels": font_pixels, "ranked_candidates": [r["metadata"] for r in ranked],
        "samples": placements, "ranking": "62 alphanumeric glyphs, normalized alpha MSE after source-ink-box fitting",
        "limitation": "Similarity ranking does not identify the original typeface. Software preview omits native text effects."}, indent=2) + "\n", encoding="utf-8")


def build(args) -> dict:
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
    ranked = candidates(args.fonts, sources)
    if not ranked:
        raise ValueError("No suitable installed font could render every verified character")
    selected = ranked[0]
    print("Similarity ranking (lower is closer to original shapes):")
    for candidate in ranked:
        print(json.dumps(candidate["metadata"]))
    vector = replace_cells(conservative, sources, selected["cells"])
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
        record["method"] = "verified Latin vector cells plus conservative unmapped cells" if index < 2 else "conservative pack, byte-identical"
        records.append(record)
    pack = {"version": 1, "generator": "tmloader/build_vector_font_pack.py",
            "status": "deliberate Latin typeface replacement; original face not identified",
            "base_pack_sha256": digest(manifest_bytes), "font": selected["metadata"],
            "rasterization": "256px local vector rendering, ink crop fitted into each original glyph ink box at 4x",
            "mapped_cells": 95, "empty_mapped_cells": 1,
            "unmapped_cells": "all preserved exactly from conservative pack",
            "textures": records}
    (output / "pack.json").write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    reviews(output, originals, conservative, vector, sources, ranked)
    print(f"Selected {selected['metadata']['family']} {selected['metadata']['style']} ({selected['metadata']['character_input']}); saved {output}")
    return pack


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--base", type=Path, default=Path("out/texture-lab/variants/fonts-4x"))
    parser.add_argument("--output", type=Path, default=Path("variants/fonts-vector-4x"))
    parser.add_argument("--fonts", type=Path, default=Path("C:/Windows/Fonts"))
    build(parser.parse_args())
