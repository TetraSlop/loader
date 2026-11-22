"""Release archive layout and loader executable checks."""

import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import build_release


def pe32(machine=0x14C, subsystem=2, characteristics=0, manifest=True):
    header = bytearray(0x200)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", header, 0x44, machine)
    struct.pack_into("<H", header, 0x58, 0x10B)
    struct.pack_into("<HH", header, 0x58 + 68, subsystem, characteristics)
    if manifest:
        header += b"<requestedExecutionLevel level='asInvoker' uiAccess='false'/>"
    return bytes(header)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def file(self, name, data=b"test"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_loader_must_be_legacy_compatible_x86_gui_executable(self):
        self.assertEqual(build_release.loader_bytes(self.file("ok.exe", pe32())), pe32())
        for name, data in {
            "x64": pe32(machine=0x8664),
            "console": pe32(subsystem=3),
            "nx": pe32(characteristics=0x100),
            "no-manifest": pe32(manifest=False),
        }.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                build_release.loader_bytes(self.file(f"{name}.exe", data))

    def test_archive_holds_only_the_loader_config_and_notices(self):
        files = build_release.package_files(b"loader")
        base = {"tmloader.exe", "tmloader.ini", "LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "retail-files.txt"}
        self.assertEqual(set(files), base)
        self.assertIn(b"runtime-dir=runtime", files["tmloader.ini"])
        self.assertIn(b"CLI11", files["THIRD_PARTY_NOTICES.txt"])
        self.assertIn(b"cpp-httplib", files["THIRD_PARTY_NOTICES.txt"])
        self.assertNotIn(b"Catch2", files["THIRD_PARTY_NOTICES.txt"])
        archive = self.root / "client.zip"
        build_release.write_archive(archive, files)
        with zipfile.ZipFile(archive) as zipped:
            self.assertEqual({name.removeprefix("TetraMaster/") for name in zipped.namelist()}, base)
            self.assertEqual(zipped.read("TetraMaster/tmloader.exe"), b"loader")
        with self.assertRaisesRegex(ValueError, "already exists"):
            build_release.write_archive(archive, {})


if __name__ == "__main__":
    unittest.main()
