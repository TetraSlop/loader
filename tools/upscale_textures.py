"""Upscale the 250 card illustrations 4x with chaiNNer's offline backend and write a reviewable pack.

chaiNNer is a separate, user-supplied checkout; none of its code is copied into tmloader.
See docs/texture-upscaling.md for setup.
"""

import argparse
import hashlib
import importlib.metadata
import json
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

# Pinned so the tiling/inference code behind recorded pack hashes stays reproducible.
CHAINNER_COMMIT = "b3ca7ff586d4071dbe8601bc4000493560503f21"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def contained(root: Path, relative: str) -> Path:
    candidate = (root / relative).resolve()
    if not candidate.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes texture lab: {relative}")
    return candidate


def bleed_transparent_rgb(rgba: np.ndarray) -> np.ndarray:
    """Fill fully transparent pixels with the nearest visible colour, leaving alpha untouched."""
    rgb, alpha = rgba[:, :, :3].copy(), rgba[:, :, 3]
    invisible = (alpha == 0).astype(np.uint8)
    if invisible.any() and not invisible.all():
        _, labels = cv2.distanceTransformWithLabels(invisible, cv2.DIST_L2, 5, labelType=cv2.DIST_LABEL_PIXEL)
        visible_colors = rgb[invisible == 0]
        rgb[invisible != 0] = visible_colors[labels[invisible != 0] - 1]
    return rgb


def verify_chainner(checkout: Path) -> None:
    """Check the pinned chaiNNer revision and make its backend importable."""
    revision = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    if revision != CHAINNER_COMMIT:
        raise ValueError(f"Expected chaiNNer {CHAINNER_COMMIT}, got {revision}")
    sys.path.insert(0, str(checkout / "backend/src"))


def load_upscaler(args: argparse.Namespace) -> Callable[[np.ndarray], np.ndarray]:
    """Return an RGBA -> 4x RGBA upscaler: model RGB through chaiNNer's PyTorch tiling, bilinear alpha."""
    verify_chainner(args.chainner.resolve())
    import torch
    from nodes.impl.pytorch.auto_split import pytorch_auto_split
    from nodes.impl.upscale.tiler import MaxTileSize
    from spandrel import ImageModelDescriptor, ModelLoader

    torch.set_num_threads(args.threads)
    device = torch.device(args.device)
    model = ModelLoader(device=device).load_from_file(str(args.model)).eval()
    if not isinstance(model, ImageModelDescriptor) or (model.scale, model.input_channels, model.output_channels) != (
        4,
        3,
        3,
    ):
        raise ValueError("A 4x RGB image model is required")

    def upscale(rgba: np.ndarray) -> np.ndarray:
        # chaiNNer works on BGR floats in 0..1; transparent RGB is filled first to avoid dark fringes.
        bgr = np.ascontiguousarray(bleed_transparent_rgb(rgba)[:, :, ::-1], dtype=np.float32) / 255
        result = pytorch_auto_split(bgr, model, device, False, MaxTileSize(128))
        result = np.rint(np.clip(result[:, :, ::-1], 0, 1) * 255).astype(np.uint8)
        # The model only sees RGB; alpha is resized deterministically so silhouettes never change.
        alpha = cv2.resize(rgba[:, :, 3], (rgba.shape[1] * 4, rgba.shape[0] * 4), interpolation=cv2.INTER_LINEAR)
        return np.dstack((result, alpha))

    return upscale


def add_model_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--chainner", type=Path, default=Path("out/hd-work/chaiNNer"))
    parser.add_argument("--model", type=Path, default=Path("out/hd-work/models/RealESRGAN_x4plus.pth"))
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--threads", type=int, choices=range(1, 33), default=4)


def build(args: argparse.Namespace) -> dict:
    upscale = load_upscaler(args)
    root = args.lab.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    textures = {entry["id"]: entry for entry in manifest["textures"]}
    output = contained(root, args.output.as_posix())
    if output == root or output.is_relative_to(root / "original"):
        raise ValueError("Output must be a separate directory inside the texture lab")
    ids = args.ids or [key for key in textures if key.rsplit("/", 1)[1].startswith("cad")]
    records = []
    for texture_id in ids:
        entry = textures[texture_id]
        if entry["font_atlas"]:
            raise ValueError("Font atlases need a glyph-aware workflow; excluded from AI upscaling")
        source = contained(root, entry["file"])
        if digest(source.read_bytes()) != entry["sha256"]:
            raise ValueError(f"Original export changed: {texture_id}")
        rgba = np.array(Image.open(source).convert("RGBA"))
        final = Image.fromarray(upscale(rgba))
        destination = contained(output, texture_id + ".png")
        destination.parent.mkdir(parents=True, exist_ok=True)
        final.save(destination)
        comparison = Image.new("RGB", (final.width * 2, final.height + 28), (40, 40, 40))
        draw = ImageDraw.Draw(comparison)
        draw.text((6, 7), "Original: 4x bilinear", fill="white")
        draw.text((final.width + 6, 7), "chaiNNer: 4x AI RGB / linear alpha", fill="white")
        baseline = Image.fromarray(rgba).resize(final.size, Image.Resampling.BILINEAR)
        comparison.paste(baseline, (0, 28), baseline)
        comparison.paste(final, (final.width, 28), final)
        comparison_path = output / "comparisons" / (texture_id.replace("/", "_") + ".png")
        comparison_path.parent.mkdir(parents=True, exist_ok=True)
        comparison.save(comparison_path)
        records.append(
            {
                "id": texture_id,
                "source_sha256": entry["sha256"],
                "logical_size": [entry["width"], entry["height"]],
                "physical_size": list(final.size),
                "file": destination.relative_to(output).as_posix(),
                "sha256": digest(destination.read_bytes()),
            }
        )
        print(f"{texture_id} {entry['width']}x{entry['height']} -> {final.width}x{final.height}", flush=True)
    pack = {
        "version": 1,
        "chainner_commit": CHAINNER_COMMIT,
        "model_file": args.model.name,
        "model_sha256": digest(args.model.read_bytes()),
        "dependencies": {
            name: importlib.metadata.version(name)
            for name in ("torch", "torchvision", "spandrel", "numpy", "opencv-python-headless", "Pillow")
        },
        "textures": records,
    }
    (output / "pack.json").write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    return pack


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_model_arguments(parser)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--output", type=Path, default=Path("cards-real-esrgan"), help="directory relative to --lab")
    parser.add_argument("ids", nargs="*", help="texture IDs to process instead of every card illustration")
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
