"""Validate reviewed samples and assemble a new, separate runtime texture pack."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

from upscale_textures import contained


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def assemble(args):
    destination = args.output.resolve()
    if destination.exists():
        raise ValueError("Output already exists; choose a new directory to preserve existing files")
    lab = args.lab.resolve()
    originals = json.loads((lab / "manifest.json").read_text(encoding="utf-8"))
    lookup = {entry["id"]: entry for entry in originals["textures"]}
    records, images, sources, seen = [], [], [], set()
    for folder in args.packs:
        folder = folder.resolve()
        manifest_bytes = (folder / "pack.json").read_bytes()
        pack = json.loads(manifest_bytes)
        if pack.get("version") != 1:
            raise ValueError(f"Unsupported pack version: {folder}")
        sources.append({"manifest": str(folder / "pack.json"),
                        "manifest_sha256": digest(manifest_bytes),
                        **{key: value for key, value in pack.items() if key != "textures"}})
        for entry in pack["textures"]:
            if entry["id"] in args.exclude:
                continue
            if entry["id"] in seen:
                raise ValueError(f"Duplicate selected image: {entry['id']}")
            original = lookup[entry["id"]]
            source = contained(lab, original["file"]).read_bytes()
            if digest(source) != original["sha256"] or entry["source_sha256"] != original["sha256"]:
                raise ValueError(f"Source hash mismatch: {entry['id']}")
            path = contained(folder, entry["file"])
            data = path.read_bytes()
            if digest(data) != entry["sha256"]:
                raise ValueError(f"Replacement hash mismatch: {entry['id']}")
            logical = [original["width"], original["height"]]
            with Image.open(path) as image:
                image.load()
                physical = list(image.size)
            scale = physical[0] // logical[0]
            if entry["logical_size"] != logical or entry["physical_size"] != physical or scale not in (2, 3, 4) or physical != [size * scale for size in logical]:
                raise ValueError(f"Invalid replacement dimensions: {entry['id']}")
            file = entry["id"] + ".png"
            target = contained(destination, file)
            records.append({**entry, "file": file})
            images.append((target, data))
            seen.add(entry["id"])
    if not records:
        raise ValueError("No textures selected")
    destination.mkdir(parents=True)
    for target, data in images:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    manifest = {"version": 1, "generator": "tmloader/assemble_texture_pack", "sources": sources, "textures": records}
    (destination / "pack.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(records)} HD images ({sum(len(data) for _, data in images):,} bytes) in {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packs", nargs="+", type=Path)
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--output", type=Path, default=Path("runtime/textures"))
    parser.add_argument("--exclude", action="append", default=[])
    assemble(parser.parse_args())
