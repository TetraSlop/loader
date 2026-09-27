"""Build a small, reviewable 4x texture pack using chaiNNer's offline backend.

The user-selected checkout stays external to tmloader; none of chaiNNer's GPL
code is copied into the loader. See docs/texture-upscaling.md for setup.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import subprocess
import sys
import time
from pathlib import Path

CHAINNER_COMMIT = "b3ca7ff586d4071dbe8601bc4000493560503f21"
SAMPLE_IDS = (
    "gW001/1000/cad00000",
    "gW001/1002/cad00020",
    "gW201/1000/menu_bg0",
    "gW000/0090/chat1",
)


def contained(root: Path, relative: str) -> Path:
    candidate = (root / relative).resolve()
    if not candidate.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes texture lab: {relative}")
    return candidate


def bleed_transparent_rgb(rgba):
    """Extend nearest visible colour under transparency, without changing alpha."""
    import cv2
    import numpy as np

    rgb, alpha = rgba[:, :, :3].copy(), rgba[:, :, 3]
    invisible = (alpha == 0).astype(np.uint8)
    if invisible.any() and not invisible.all():
        _, labels = cv2.distanceTransformWithLabels(
            invisible, cv2.DIST_L2, 5, labelType=cv2.DIST_LABEL_PIXEL
        )
        visible_colors = rgb[invisible == 0]
        rgb[invisible != 0] = visible_colors[labels[invisible != 0] - 1]
    return rgb


def build(args) -> dict:
    checkout = args.chainner.resolve()
    revision = subprocess.check_output(
        ["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != CHAINNER_COMMIT:
        raise ValueError(f"Expected chaiNNer {CHAINNER_COMMIT}, got {revision}")
    sys.path.insert(0, str(checkout / "backend/src"))
    # Execute the actual upstream inference/tiling implementation used by the
    # chaiNNer PyTorch Upscale Image node, without starting Electron or a server.
    import cv2
    import numpy as np
    import torch
    from nodes.impl.pytorch.auto_split import pytorch_auto_split
    from nodes.impl.upscale.tiler import MaxTileSize
    from PIL import Image, ImageDraw
    from spandrel import ImageModelDescriptor, ModelLoader

    torch.set_num_threads(args.threads)
    device = torch.device(args.device)
    model_hash = hashlib.sha256(args.model.read_bytes()).hexdigest()
    model = ModelLoader(device=device).load_from_file(str(args.model)).eval()
    if not isinstance(model, ImageModelDescriptor) or (
        model.scale,
        model.input_channels,
        model.output_channels,
    ) != (4, 3, 3):
        raise ValueError("This sample workflow requires a 4x RGB image model")
    root = args.lab.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    textures = {entry["id"]: entry for entry in manifest["textures"]}
    output = root / "sample-4x"
    output.mkdir(exist_ok=True)
    records = []
    for texture_id in args.ids or SAMPLE_IDS:
        entry = textures[texture_id]
        if entry["font_atlas"]:
            raise ValueError(
                "Font atlases need a glyph-aware workflow; excluded from AI upscaling"
            )
        source = contained(root, entry["file"])
        if hashlib.sha256(source.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError(f"Original export changed: {texture_id}")
        rgba = np.array(Image.open(source).convert("RGBA"))
        rgb = bleed_transparent_rgb(rgba)
        # chaiNNer images use BGR floats. Its inference adapter converts to RGB
        # for the model and back afterwards.
        bgr = np.ascontiguousarray(rgb[:, :, ::-1], dtype=np.float32) / 255
        started = time.monotonic()
        result = pytorch_auto_split(bgr, model, device, False, MaxTileSize(128))
        result = np.rint(np.clip(result[:, :, ::-1], 0, 1) * 255).astype(np.uint8)
        expected = (rgba.shape[0] * 4, rgba.shape[1] * 4)
        if result.shape[:2] != expected:
            raise ValueError(f"Unexpected output dimensions for {texture_id}")
        # Keep transparency deterministic; the neural model never invents alpha.
        alpha = cv2.resize(
            rgba[:, :, 3], (expected[1], expected[0]), interpolation=cv2.INTER_LINEAR
        )
        final = Image.fromarray(np.dstack((result, alpha)))
        destination = contained(output, texture_id + ".png")
        destination.parent.mkdir(parents=True, exist_ok=True)
        final.save(destination)
        comparison = Image.new(
            "RGB", (final.width * 2, final.height + 28), (40, 40, 40)
        )
        draw = ImageDraw.Draw(comparison)
        draw.text((6, 7), "Original: 4x bilinear", fill="white")
        draw.text(
            (final.width + 6, 7), "chaiNNer: 4x AI RGB / linear alpha", fill="white"
        )
        baseline = Image.fromarray(rgba).resize(final.size, Image.Resampling.BILINEAR)
        comparison.paste(baseline, (0, 28), baseline)
        comparison.paste(final, (final.width, 28), final)
        comparison_path = contained(
            root / "comparisons", texture_id.replace("/", "_") + ".png"
        )
        comparison_path.parent.mkdir(parents=True, exist_ok=True)
        comparison.save(comparison_path)
        records.append(
            {
                "id": texture_id,
                "source_sha256": entry["sha256"],
                "logical_size": [entry["width"], entry["height"]],
                "physical_size": list(final.size),
                "file": destination.relative_to(output).as_posix(),
                "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
                "seconds": round(time.monotonic() - started, 3),
            }
        )
        print(
            f"Upscaled {texture_id}: {entry['width']}x{entry['height']} -> {final.width}x{final.height}",
            flush=True,
        )
    pack = {
        "version": 1,
        "status": "offline sample; not installed in tmloader",
        "chainner_commit": revision,
        "model_file": args.model.name,
        "model_sha256": model_hash,
        "dependencies": {
            name: importlib.metadata.version(name)
            for name in (
                "torch",
                "torchvision",
                "spandrel",
                "numpy",
                "opencv-python-headless",
                "Pillow",
            )
        },
        "alpha": "original coverage enlarged 4x with bilinear interpolation",
        "textures": records,
    }
    (output / "pack.json").write_text(
        json.dumps(pack, indent=2) + "\n", encoding="utf-8"
    )
    return pack


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chainner", type=Path, default=Path("out/hd-work/chaiNNer"))
    parser.add_argument(
        "--model",
        type=Path,
        default=Path("out/hd-work/models/RealESRGAN_x4plus_anime_6B.pth"),
    )
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--threads", type=int, choices=range(1, 33), default=4)
    parser.add_argument("ids", nargs="*")
    build(parser.parse_args())


if __name__ == "__main__":
    main()
