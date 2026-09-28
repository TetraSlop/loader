"""Synthetic archive tests; no proprietary assets or native game are required."""

import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from extract_textures import parse_archive, png_bytes
from upscale_textures import contained


def chunk(name, data=b""):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def image():
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff"))
        + chunk(b"IEND")
    )


def entry(kind, name, offset, size, flags=0, unpacked=0):
    return struct.pack("<I12s4I", kind, name, offset, size, flags, unpacked)


def archive(name=b"sample"):
    png = image()
    padded = png + bytes((-len(png)) % 16)
    directory = entry(0x4000, name, 32, len(padded), 2, len(padded)) + padded
    return entry(0x8000, b"0090", 32, len(directory)) + directory


class TextureToolsTests(unittest.TestCase):
    def test_nested_identity_and_padding(self):
        (texture,) = parse_archive(archive())
        self.assertEqual(texture.name, "0090/sample")
        self.assertEqual(texture.offset, 64)
        self.assertEqual(texture.png, image())
        self.assertEqual(texture.record_size % 16, 0)

    def test_png_crc_is_checked(self):
        for offset in (20, 42):  # IHDR and IDAT payloads
            corrupt = bytearray(image())
            corrupt[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                png_bytes(bytes(corrupt))

    def test_truncated_png_is_rejected(self):
        with self.assertRaises(ValueError):
            png_bytes(image()[:-1])

    def test_archive_truncation_is_rejected(self):
        with self.assertRaises(ValueError):
            parse_archive(archive()[:-1])

    def test_unsafe_names_are_rejected(self):
        for name in (b"../sample", b"C:sample", b"..", b"/sample"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                parse_archive(archive(name))

    def test_unknown_compression_is_rejected(self):
        data = bytearray(archive())
        struct.pack_into("<I", data, 32 + 24, 1)
        with self.assertRaisesRegex(ValueError, "compressed"):
            parse_archive(bytes(data))

    def test_entry_cannot_point_inside_directory(self):
        data = bytearray(archive())
        struct.pack_into("<I", data, 32 + 16, 16)
        with self.assertRaises(ValueError):
            parse_archive(bytes(data))

    def test_output_path_cannot_escape_root(self):
        root = Path("out/texture-lab").resolve()
        with self.assertRaises(ValueError):
            contained(root, "../unexpected.png")
        self.assertEqual(contained(root, "pack/card.png"), root / "pack/card.png")


if __name__ == "__main__":
    unittest.main()
