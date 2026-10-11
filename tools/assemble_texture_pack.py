"""Validate the reviewed texture-lab packs and merge them into the loader's texture pack (assets/runtime/textures)."""

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

from upscale_textures import contained, digest

PACKS = ("cards-real-esrgan", "variants/ui-complete", "variants/fonts-vector-4x-v2")
# The loader reads only these fields; authoring metadata and build paths stay in the texture lab.
FIELDS = ("id", "source_sha256", "sha256", "file", "logical_size", "physical_size")


def assemble(lab: Path, packs: list[Path], output: Path) -> None:
    destination = output.resolve()
    if destination.exists():
        raise ValueError("Output already exists; choose a new directory to preserve existing files")
    lab = lab.resolve()
    originals = json.loads((lab / "manifest.json").read_text(encoding="utf-8"))
    lookup = {entry["id"]: entry for entry in originals["textures"]}
    records, images, seen = [], [], set()
    for folder in packs:
        folder = (lab / folder).resolve()
        pack = json.loads((folder / "pack.json").read_bytes())
        if pack.get("version") != 1:
            raise ValueError(f"Unsupported pack version: {folder}")
        for entry in pack["textures"]:
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
            if (
                entry["logical_size"] != logical
                or entry["physical_size"] != physical
                or scale not in (2, 3, 4)
                or physical != [size * scale for size in logical]
            ):
                raise ValueError(f"Invalid replacement dimensions: {entry['id']}")
            file = entry["id"] + ".png"
            records.append({key: file if key == "file" else entry[key] for key in FIELDS})
            images.append((contained(destination, file), data))
            seen.add(entry["id"])
    if not records:
        raise ValueError("No textures selected")
    destination.mkdir(parents=True)
    for target, data in images:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    manifest = {"version": 1, "textures": records}
    (destination / "pack.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Prepared {len(records)} HD images ({sum(len(data) for _, data in images):,} bytes) in {destination}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packs", nargs="*", type=Path, default=PACKS, help="pack folders relative to --lab")
    parser.add_argument("--lab", type=Path, default=Path("out/texture-lab"))
    parser.add_argument("--output", type=Path, default=Path("assets/runtime/textures"), help="new directory")
    args = parser.parse_args()
    try:
        assemble(args.lab, args.packs, args.output)
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
