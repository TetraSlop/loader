"""Read TM's nested gW archives and export lossless, named PNG textures.

No game code is executed and no source files are changed. The archive uses
32-byte directory entries with offsets relative to the containing directory.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SAFE_NAME = re.compile(r"[A-Za-z0-9_-]+\Z")


@dataclass(frozen=True)
class Texture:
    name: str
    offset: int
    record_size: int
    png: bytes


def png_bytes(data: bytes) -> bytes:
    """Validate chunk boundaries/CRCs and omit archive alignment after IEND."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("Texture is not a PNG")
    offset = 8
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        end = offset + 12 + size
        if end > len(data):
            raise ValueError("PNG chunk extends beyond its archive entry")
        tag = data[offset + 4 : offset + 8]
        actual = zlib.crc32(data[offset + 4 : end - 4])
        expected = struct.unpack_from(">I", data, end - 4)[0]
        if actual != expected:
            raise ValueError("PNG chunk checksum mismatch")
        if tag == b"IEND":
            if size:
                raise ValueError("Invalid PNG IEND")
            return data[:end]
        offset = end
    raise ValueError("PNG has no complete IEND")


def parse_archive(data: bytes) -> list[Texture]:
    textures: list[Texture] = []
    seen: set[str] = set()

    def directory(base: int, size: int, prefix: str, depth: int = 0) -> None:
        if depth > 8 or size < 32 or base + size > len(data):
            raise ValueError("Invalid archive directory bounds")
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
            offset, length, flags, unpacked = struct.unpack_from(
                "<4I", data, record + 16
            )
            if offset < previous_end or not length or offset + length > size:
                raise ValueError("Overlapping or out-of-range archive entry")
            previous_end = offset + length
            if kind == 0x8000:
                directory(base + offset, length, name + "/", depth + 1)
            elif kind == 0x4000:
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
    from PIL import Image

    data_dir, output_dir = data_dir.resolve(), output_dir.resolve()
    # The default source is runtime/data. Never write elsewhere in that runtime.
    if output_dir.is_relative_to(data_dir.parent):
        raise ValueError("Export directory must be outside the source runtime")
    archives = sorted(data_dir.glob("gW*.dat"))
    if not archives:
        raise ValueError(f"No gW archives in {data_dir}")
    entries = []
    sources = []
    for archive in archives:
        data = archive.read_bytes()
        sources.append(
            {"file": archive.name, "sha256": hashlib.sha256(data).hexdigest()}
        )
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
                        "sha256": hashlib.sha256(texture.png).hexdigest(),
                        "width": image.width,
                        "height": image.height,
                        "source_mode": image.mode,
                        "alpha_range": list(alpha.getextrema()),
                        "font_atlas": texture.name.rsplit("/", 1)[-1].startswith(
                            "font_"
                        ),
                    }
                )
    manifest = {"version": 1, "archives": sources, "textures": entries}
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=Path("runtime/data"))
    parser.add_argument("--output", type=Path, default=Path("out/texture-lab"))
    args = parser.parse_args()
    manifest = export(args.data, args.output)
    print(
        f"Exported {len(manifest['textures'])} textures from {len(manifest['archives'])} archives to {args.output}"
    )


if __name__ == "__main__":
    main()
