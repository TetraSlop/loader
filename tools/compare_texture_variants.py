"""Make six labeled, equally sized offline comparisons from chaiNNer samples."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from upscale_textures import bleed_transparent_rgb, contained


VARIANTS = (
    ("original", "A / Original enlarged", "Bilinear resize; reference"),
    ("crisp", "B / Crisp resize", "Lanczos + mild sharpening; no AI"),
    ("anime", "C / Anime AI", "RealESRGAN anime 6B; first attempt"),
    ("general", "D / General AI", "RealESRGAN x4plus"),
    ("restoration", "E / Restoration AI", "RealESRNet x4plus"),
    ("blend", "F / Light AI blend", "25% general AI + 75% crisp resize"),
)
SAMPLES = ("gW001/1000/cad00000", "gW001/1002/cad00020")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_sample(root: Path, texture_id: str, source_hash: str):
    pack = json.loads((root / "pack.json").read_text(encoding="utf-8"))
    entry = next(item for item in pack["textures"] if item["id"] == texture_id)
    path = contained(root, entry["file"])
    if entry["source_sha256"] != source_hash or digest(path) != entry["sha256"]:
        raise ValueError(f"Sample does not match its source/manifest: {path}")
    with Image.open(path) as image:
        return image.convert("RGBA"), {
            "model_file": pack["model_file"],
            "model_sha256": pack["model_sha256"],
            "chainner_commit": pack["chainner_commit"],
            "pack": str((root / "pack.json").resolve()),
        }


def font(size: int):
    windows_font = Path("C:/Windows/Fonts/segoeui.ttf")
    if windows_font.exists():
        return ImageFont.truetype(str(windows_font), size)
    return ImageFont.load_default(size=size)


def comparison_sheet(texture_id: str, images: dict[str, Image.Image]):
    width, height = images["original"].size
    padding, header, label = 16, 94, 66
    cell_width, cell_height = width + padding * 2, height + label + padding
    canvas = Image.new("RGB", (cell_width * 3, header + cell_height * 2), "#111820")
    draw = ImageDraw.Draw(canvas)
    draw.text((padding, 13), "TETRA MASTER / SIX TEXTURE TREATMENTS", font=font(23), fill="white")
    draw.text((padding, 47), f"{texture_id}  |  72 x 96 source, shown at 4x", font=font(16), fill="#c0ccd7")
    draw.text((padding, 69), "Same original alpha in every panel. Offline artwork preview; not an in-game screenshot.", font=font(14), fill="#8da2b5")
    for index, (key, title, caption) in enumerate(VARIANTS):
        x = (index % 3) * cell_width
        y = header + (index // 3) * cell_height
        draw.rectangle((x + 5, y + 5, x + cell_width - 5, y + cell_height - 5), fill="#202a35")
        draw.text((x + padding, y + 12), title, font=font(19), fill="white")
        draw.text((x + padding, y + 39), caption, font=font(14), fill="#b8c8d5")
        draw.rectangle((x + padding, y + label, x + padding + width - 1, y + label + height - 1), fill=(40, 40, 40))
        canvas.paste(images[key], (x + padding, y + label), images[key])
    return canvas


def build(lab: Path):
    lab = lab.resolve()
    output = lab / "variant-review"
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((lab / "manifest.json").read_text(encoding="utf-8"))
    textures = {item["id"]: item for item in manifest["textures"]}
    records = []
    for texture_id in SAMPLES:
        entry = textures[texture_id]
        source = contained(lab, entry["file"])
        if digest(source) != entry["sha256"]:
            raise ValueError(f"Source export changed: {source}")
        with Image.open(source) as image:
            rgba = np.array(image.convert("RGBA"))
        size = (entry["width"] * 4, entry["height"] * 4)
        rgb = Image.fromarray(bleed_transparent_rgb(rgba))
        alpha = Image.fromarray(cv2.resize(rgba[:, :, 3], size, interpolation=cv2.INTER_LINEAR))
        original = rgb.resize(size, Image.Resampling.BILINEAR)
        crisp = rgb.resize(size, Image.Resampling.LANCZOS).filter(
            ImageFilter.UnsharpMask(radius=1.2, percent=90, threshold=3)
        )
        images = {"original": original, "crisp": crisp}
        models = {}
        for key, directory in (
            ("anime", "sample-4x"),
            ("general", "variants/real-esrgan"),
            ("restoration", "variants/real-esrnet"),
        ):
            result, models[key] = read_sample(lab / directory, texture_id, entry["sha256"])
            if result.size != size or not np.array_equal(np.array(result.getchannel("A")), np.array(alpha)):
                raise ValueError(f"Size/alpha mismatch for {texture_id}: {key}")
            images[key] = result.convert("RGB")
        images["blend"] = Image.blend(crisp, images["general"], 0.25)
        destinations = {}
        for key, _, _ in VARIANTS:
            images[key].putalpha(alpha)
            destination = contained(output, key + "/" + texture_id + ".png")
            destination.parent.mkdir(parents=True, exist_ok=True)
            images[key].save(destination)
            destinations[key] = {
                "file": destination.relative_to(output).as_posix(),
                "sha256": digest(destination),
            }
        sheet = output / (texture_id.replace("/", "_") + ".png")
        comparison_sheet(texture_id, images).save(sheet)
        records.append({
            "id": texture_id,
            "source_sha256": entry["sha256"],
            "logical_size": [entry["width"], entry["height"]],
            "physical_size": list(size),
            "comparison": sheet.name,
            "variants": destinations,
            "models": models,
        })
        print(sheet, flush=True)
    (output / "review.json").write_text(json.dumps({
        "status": "offline previews; not installed in the game",
        "alpha": "Identical original coverage resized 4x with cv2.INTER_LINEAR",
        "crisp": "Pillow Lanczos RGB, UnsharpMask(radius=1.2, percent=90, threshold=3)",
        "blend": "25% RealESRGAN_x4plus RGB + 75% crisp RGB in sRGB",
        "variants": [{"key": key, "title": title, "description": caption} for key, title, caption in VARIANTS],
        "textures": records,
    }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    build(parser.parse_args().lab)
