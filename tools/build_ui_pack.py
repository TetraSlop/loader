"""Build the 4x UI and portrait texture pack, keeping every native sprite coordinate.

Illustrations go through the AI model ("D", RealESRGAN x4plus by default); audited text, icons and cursors are
redrawn as vectors; remaining symbols get deterministic filtering. Tiled backgrounds are joined before upscaling
and split afterwards so tile seams match.
"""

import argparse
import json
import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

from cursor_art import vector_cursor
from title_glyphs import (
    refine_pol_atlas,
    refine_status_counter_atlas,
    refine_status_icons_atlas,
    refine_title_atlas,
)
from ui_glyphs import MENU_LABELS, menu_labels, vector_stats
from upscale_textures import (
    CHAINNER_COMMIT,
    add_model_arguments,
    bleed_transparent_rgb,
    contained,
    digest,
    load_upscaler,
)

# Installed Windows fonts for the vector substitutes; hashes are recorded and no font is copied into the pack.
FONTS = {
    "menu": "arialbi.ttf",
    "stat": "ariblk.ttf",
    "title": "georgiab.ttf",
    "toolbar": "arialbd.ttf",
    "counter": "arial.ttf",
}

# Illustrated sprites, visually audited across every occurrence in the original pack.
ILLUSTRATIONS = {
    "mes_type",
    "chat1",
    "arrow",
    "base_r",
    "base_g",
    "base_b",
    "base_st1",
    "base_st2",
    "through",
    "name",
    "hn",
    "mog",
    "guild",
    "flame",
    "comment",
    "badge",
}

# Illustrated regions of mixed sheets; the rest of each sheet keeps deterministic filtering.
ILLUSTRATED_REGIONS = {
    "ttl_menu": [(0, 0, 158, 64)],
    "face_bd": [(0, 0, 154, 112)],
}

# (label, group, tile prefix, tile suffixes, columns, lettering rectangles protected from the AI pass).
# Tile order follows the native draw rectangles.
BACKGROUNDS = (
    ("menu", "gW101/1003", "menu_bg", "012345", 3, []),
    ("back", "gW101/1002", "back", "012345", 3, [(247, 182, 474, 252)]),
    ("title", "gW201/1001", "title_", "012345", 3, []),
    ("board4", "gW101/1005", "masu_4", "abcd", 2, [(72, 184, 302, 255)]),
    ("board5", "gW101/1006", "masu_5", "abcd", 2, [(72, 184, 302, 255)]),
)


def conservative(rgba: np.ndarray) -> np.ndarray:
    """Deterministic 4x: Lanczos RGB with mild unsharp masking, Catmull-Rom alpha."""
    from nodes.impl.resize import ResizeFilter, resize

    size = (rgba.shape[1] * 4, rgba.shape[0] * 4)
    rgb = bleed_transparent_rgb(rgba).astype(np.float32) / 255
    rgb = resize(rgb, size, ResizeFilter.LANCZOS, separate_alpha=True)
    rgb += 0.2 * (rgb - cv2.GaussianBlur(rgb, (0, 0), 1.0))
    # Clamp alpha to the source range so partly transparent symbols stay partly transparent.
    alpha = resize(
        rgba[:, :, 3].astype(np.float32) / 255,
        size,
        ResizeFilter.CATROM,
        separate_alpha=True,
    )
    alpha = np.clip(alpha, rgba[:, :, 3].min() / 255, rgba[:, :, 3].max() / 255)
    return np.rint(np.clip(np.dstack((rgb, alpha)), 0, 1) * 255).astype(np.uint8)


def protected_background(
    rgba: np.ndarray, regions: list[tuple[int, int, int, int]], ai: Callable[[np.ndarray], np.ndarray]
) -> np.ndarray:
    """Upscale a joined background with AI, blending protected lettering regions from the deterministic path."""
    result = ai(rgba)
    if not regions:
        return result
    stable = conservative(rgba)
    mask = np.zeros(result.shape[:2], np.float32)
    for left, top, right, bottom in regions:
        mask[top * 4 : bottom * 4, left * 4 : right * 4] = 1
    # Feather outside the protected rectangles, never into their lettering.
    outer = cv2.GaussianBlur(mask, (0, 0), 10)
    mask = np.maximum(mask, outer)[:, :, None]
    return np.rint(result * (1 - mask) + stable * mask).astype(np.uint8)


def preview(path: Path, original: np.ndarray, result: np.ndarray, title: str) -> None:
    before = Image.fromarray(original).resize((result.shape[1], result.shape[0]), Image.Resampling.BILINEAR)
    after = Image.fromarray(result)
    # Only the preview is downscaled.
    factor = min(1, 1152 / max(after.width, after.height))
    size = (round(after.width * factor), round(after.height * factor))
    before = before.resize(size, Image.Resampling.LANCZOS)
    after = after.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (size[0] * 2, size[1] + 52), (38, 42, 49))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 17)
    draw.text((8, 5), title, fill="white", font=font)
    draw.text((8, 29), "Original enlarged", fill="white", font=font)
    draw.text((size[0] + 8, 29), "UI pack", fill="white", font=font)
    canvas.paste(before, (0, 52), before)
    canvas.paste(after, (size[0], 52), after)
    path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(path)


def build(args: argparse.Namespace) -> dict:
    root = args.lab.resolve()
    output = contained(root, args.output.as_posix())
    if output == root or output.is_relative_to(root / "original"):
        raise ValueError("Output must be separate from the original exports")
    output.mkdir(parents=True, exist_ok=True)
    entries = json.loads((root / "manifest.json").read_text(encoding="utf-8"))["textures"]
    lookup = {entry["id"]: entry for entry in entries}
    selected = [entry for entry in entries if not re.fullmatch(r"(?:cad\d+|font_\d+)", entry["id"].rsplit("/", 1)[-1])]
    if len(selected) != 211:
        raise ValueError(f"Expected the audited 181 UI assets and 30 portraits, got {len(selected)}")
    ai = load_upscaler(args)
    # Fail before expensive inference if chaiNNer's native resize extension is missing.
    from nodes.impl.resize import resize  # noqa: F401

    fonts = {role: args.fonts / name for role, name in FONTS.items()}
    font_hashes = {role: digest(path.read_bytes()) for role, path in fonts.items()}
    cache, methods = {}, {}

    def original(entry):
        file = contained(root, entry["file"])
        if digest(file.read_bytes()) != entry["sha256"]:
            raise ValueError(f"Original changed: {entry['id']}")
        with Image.open(file) as image:
            return np.array(image.convert("RGBA"))

    # Resume a previous run: reuse outputs whose hashes still match this workflow's manifest and model.
    old_manifest = output / "pack.json"
    model_hash = digest(args.model.read_bytes())
    if old_manifest.exists():
        old = json.loads(old_manifest.read_text(encoding="utf-8"))
        if old.get("workflow_version") == 1 and old.get("model_sha256") == model_hash:
            for item in old["textures"]:
                path = contained(output, item["file"])
                if path.exists() and digest(path.read_bytes()) == item["sha256"]:
                    cache[item["source_sha256"]] = np.array(Image.open(path).convert("RGBA"))
                    methods[item["source_sha256"]] = item["method"]

    for label, group, prefix, suffixes, columns, protected in BACKGROUNDS:
        parts = [lookup[f"{group}/{prefix}{suffix}"] for suffix in suffixes]
        tiles = [original(part) for part in parts]
        width = sum(tile.shape[1] for tile in tiles[:columns])
        height = tiles[0].shape[0] + tiles[columns].shape[0]
        joined = np.zeros((height, width, 4), np.uint8)
        rectangles = []
        for index, tile in enumerate(tiles):
            x, y = (index % columns) * 256, (index // columns) * 256
            h, w = tile.shape[:2]
            joined[y : y + h, x : x + w] = tile
            rectangles.append((x, y, w, h))
        print(f"Joined background {label}: {width}x{height}", flush=True)
        # The cached joined result is keyed on its inputs, so changing protected regions invalidates it.
        background_file = output / "cache" / f"{label}.png"
        background_meta = background_file.with_suffix(".json")
        expected = {
            "version": 1,
            "source": digest(joined.tobytes()),
            "model": model_hash,
            "protected": [list(rect) for rect in protected],
        }
        if background_meta.exists() and background_file.exists():
            saved = json.loads(background_meta.read_text(encoding="utf-8"))
            valid = saved.get("inputs") == expected and saved.get("sha256") == digest(background_file.read_bytes())
        else:
            valid = False
        if valid:
            result = np.array(Image.open(background_file).convert("RGBA"))
        else:
            result = protected_background(joined, protected, ai)
            background_file.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(result).save(background_file)
            background_meta.write_text(
                json.dumps({"inputs": expected, "sha256": digest(background_file.read_bytes())}),
                encoding="utf-8",
            )
        for part, (x, y, w, h) in zip(parts, rectangles):
            cache[part["sha256"]] = result[y * 4 : (y + h) * 4, x * 4 : (x + w) * 4].copy()
            methods[part["sha256"]] = (
                "D on assembled background; original lettering protected" if protected else "D on assembled background"
            )
        reconstructed = np.zeros_like(result)
        for part, (x, y, w, h) in zip(parts, rectangles):
            reconstructed[y * 4 : (y + h) * 4, x * 4 : (x + w) * 4] = cache[part["sha256"]]
        if not np.array_equal(reconstructed, result):
            raise ValueError(f"Background split/reassembly differs: {label}")
        preview(output / "review" / f"background-{label}.png", joined, result, label)

    records = []
    review_names = {
        "cursor",
        "window",
        "window2",
        "on_card1",
        "point",
        "ttl_menu",
        "head",
        "on_title",
        "friend2",
        "friend3",
        "friend4",
        "fac00001",
        "fac00022",
        "fac00025",
        "face_bd",
    }
    reviewed_hashes = set()
    stat_checks = []
    cursor_checks = None
    glyph_checks = {}
    refinements = {
        "on_title": (refine_title_atlas, (fonts["title"],)),
        "friend4": (refine_pol_atlas, (fonts["toolbar"],)),
        "friend2": (refine_status_icons_atlas, ()),
        "friend3": (refine_status_counter_atlas, (fonts["counter"],)),
    }
    for index, entry in enumerate(selected):
        source = original(entry)
        key = entry["sha256"]
        name = entry["id"].rsplit("/", 1)[-1]
        if key not in cache:
            if re.fullmatch(r"fac\d+", name) or name in ILLUSTRATIONS or name.startswith("effec_"):
                cache[key], methods[key] = (
                    ai(source),
                    "D illustration RGB; original bilinear alpha",
                )
            else:
                result = conservative(source)
                method = "chaiNNer Lanczos RGB + 20% unsharp; Catmull-Rom source alpha"
                for left, top, right, bottom in ILLUSTRATED_REGIONS.get(name, []):
                    crop = source[top:bottom, left:right]
                    result[top * 4 : bottom * 4, left * 4 : right * 4] = ai(crop)
                    method = "D decorative regions; deterministic lettering and symbols"
                cache[key], methods[key] = result, method
        result = cache[key]
        if name == "ttl_menu":
            result = cache[key] = menu_labels(source, result, fonts["menu"])
            methods[key] = "D decoration; eight vector menu labels fitted to original ink rectangles"
        if name == "on_card1":
            result, stat_checks = vector_stats(source, result, fonts["stat"])
            cache[key] = result
            methods[key] = "Deterministic source; vector direction arrows and yellow 0-9 in native sprite cells"
        if name == "cursor":
            result, cursor_checks = vector_cursor(source)
            cache[key] = result
            methods[key] = "Three coordinate-preserving vector glove poses with white bevels"
        if name in refinements:
            refine, font_files = refinements[name]
            result, glyph_checks[name] = refine(source, result, *font_files)
            cache[key] = result
            methods[key] = glyph_checks[name]["method"]
        if result.shape != (source.shape[0] * 4, source.shape[1] * 4, 4):
            raise ValueError(f"Wrong replacement shape: {entry['id']}")
        path = contained(output, entry["id"] + ".png")
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(result).save(path)
        records.append(
            {
                "id": entry["id"],
                "source_sha256": key,
                "logical_size": [entry["width"], entry["height"]],
                "physical_size": [result.shape[1], result.shape[0]],
                "file": path.relative_to(output).as_posix(),
                "sha256": digest(path.read_bytes()),
                "method": methods[key],
            }
        )
        if name in review_names and key not in reviewed_hashes:
            preview(
                output / "review" / (entry["id"].replace("/", "_") + ".png"),
                source,
                result,
                entry["id"],
            )
            reviewed_hashes.add(key)
        print(f"{index + 1}/{len(selected)} {entry['id']}: {methods[key]}", flush=True)
        # Rewrite the manifest after every asset so an interrupted run can resume.
        pack = {
            "version": 1,
            "workflow_version": 1,
            "generator": "tmloader/build_ui_pack",
            "chainner_commit": CHAINNER_COMMIT,
            "model_file": args.model.name,
            "model_sha256": model_hash,
            **{f"{role}_font": path.name for role, path in fonts.items()},
            **{f"{role}_font_sha256": font_hashes[role] for role in fonts},
            "textures": records,
        }
        old_manifest.write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    report = {
        "entries": len(records),
        "unique_sources": len(cache),
        "portraits": sum(bool(re.fullmatch(r"fac\d+", x["id"].rsplit("/", 1)[-1])) for x in records),
        "background_groups": len(BACKGROUNDS),
        "font_atlases": 0,
        "menu_vector_labels": len(MENU_LABELS),
        "stat_vector_sprites": stat_checks,
        "cursor_vector_poses": cursor_checks,
        "title_toolbar_status_glyphs": glyph_checks,
        "pending": [
            "User inspection in game at 1440p and 4K",
            "Handwritten scores, event/result wordmarks and copper symbols retain filtered original contours",
        ],
    }
    (output / "audit.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return pack


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_model_arguments(parser)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--output", type=Path, default=Path("variants/ui-complete"))
    parser.add_argument("--fonts", type=Path, default=Path("C:/Windows/Fonts"), help="folder holding the FONTS files")
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
