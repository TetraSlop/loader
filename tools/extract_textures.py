"""Export the PNG textures in Tetra Master's gW*.dat archives, with a manifest, to a texture lab.

Each archive directory is a table of 32-byte entries whose offsets are relative to that directory.
"""

import argparse
import io
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

from upscale_textures import digest

# The empty IEND chunk, including its length and constant CRC, always ends a PNG.
IEND = bytes.fromhex("0000000049454e44ae426082")
SAFE_NAME = re.compile(r"[A-Za-z0-9_-]+\Z")
ENTRY_DIRECTORY = 0x8000
ENTRY_FILE = 0x4000


@dataclass(frozen=True)
class Texture:
    name: str
    offset: int
    record_size: int
    png: bytes


def png_bytes(data: bytes) -> bytes:
    """Drop archive alignment padding after IEND, then let Pillow check every chunk CRC."""
    end = data.rfind(IEND)
    if end < 0:
        raise ValueError("PNG has no complete IEND")
    png = data[: end + len(IEND)]
    try:
        with Image.open(io.BytesIO(png)) as image:
            image.verify()
    except (OSError, SyntaxError) as exc:
        raise ValueError(f"Invalid PNG: {exc}") from exc
    return png


def parse_archive(data: bytes) -> list[Texture]:
    textures: list[Texture] = []
    seen: set[str] = set()

    def directory(base: int, size: int, prefix: str, depth: int = 0) -> None:
        if depth > 8 or size < 32 or base + size > len(data):
            raise ValueError("Invalid archive directory bounds")
        # Entry layout: u32 kind, char name[12], u32 offset, u32 length, u32 flags, u32 unpacked length.
        # The first entry's offset doubles as the size of the entry table.
        table_size = struct.unpack_from("<I", data, base + 16)[0]
        if not 32 <= table_size <= size or table_size % 32:
            raise ValueError("Invalid archive directory table")
        previous_end = table_size
        for record in range(base, base + table_size, 32):
            kind = struct.unpack_from("<I", data, record)[0]
            name = data[record + 4 : record + 16].split(b"\0", 1)[0].decode("ascii")
            if not SAFE_NAME.fullmatch(name):
                raise ValueError("Unsafe archive entry name")
            name = prefix + name
            offset, length, flags, unpacked = struct.unpack_from("<4I", data, record + 16)
            if offset < previous_end or not length or offset + length > size:
                raise ValueError("Overlapping or out-of-range archive entry")
            previous_end = offset + length
            if kind == ENTRY_DIRECTORY:
                directory(base + offset, length, name + "/", depth + 1)
            elif kind == ENTRY_FILE:
                if flags != 2 or unpacked != length:
                    raise ValueError("Unsupported compressed texture entry")
                if name.casefold() in seen:
                    raise ValueError("Duplicate archive texture name")
                seen.add(name.casefold())
                png = png_bytes(data[base + offset : base + offset + length])
                textures.append(Texture(name, base + offset, length, png))
            else:
                raise ValueError(f"Unsupported archive entry type {kind:#x}")

    directory(0, len(data), "")
    return textures


def export(data_dir: Path, output_dir: Path) -> dict:
    data_dir, output_dir = data_dir.resolve(), output_dir.resolve()
    if output_dir.is_relative_to(data_dir.parent):
        raise ValueError("Export directory must be outside the source runtime")
    archives = sorted(data_dir.glob("gW*.dat"))
    if not archives:
        raise ValueError(f"No gW archives in {data_dir}")
    entries, sources = [], []
    for archive in archives:
        data = archive.read_bytes()
        sources.append({"file": archive.name, "sha256": digest(data)})
        for texture in parse_archive(data):
            relative = f"{archive.stem}/{texture.name}.png"
            destination = output_dir / "original" / relative
            if not destination.resolve().is_relative_to(output_dir):
                raise ValueError("Export destination escapes output directory")
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists() and destination.read_bytes() != texture.png:
                raise ValueError(f"Refusing to replace changed export {destination}")
            destination.write_bytes(texture.png)
            with Image.open(io.BytesIO(texture.png)) as image:
                image.load()
                alpha = image.convert("RGBA").getchannel("A")
                entries.append(
                    {
                        "id": f"{archive.stem}/{texture.name}",
                        "file": "original/" + relative,
                        "source_offset": texture.offset,
                        "source_record_size": texture.record_size,
                        "png_size": len(texture.png),
                        "sha256": digest(texture.png),
                        "width": image.width,
                        "height": image.height,
                        "source_mode": image.mode,
                        "alpha_range": list(alpha.getextrema()),
                        "font_atlas": texture.name.rsplit("/", 1)[-1].startswith("font_"),
                    }
                )
    manifest = {"version": 1, "archives": sources, "textures": entries}
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=Path("assets/runtime/data"))
    parser.add_argument("--output", type=Path, default=Path("out/texture-lab"))
    args = parser.parse_args()
    try:
        manifest = export(args.data, args.output)
    except (OSError, ValueError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    print(f"Exported {len(manifest['textures'])} textures from {len(manifest['archives'])} archives to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
