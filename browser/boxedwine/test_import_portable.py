import tempfile
import unittest
import zipfile
from pathlib import Path

from import_portable import import_client, import_server


class ImportTests(unittest.TestCase):
    def test_default_package_has_no_assets_and_runtime_state_is_not_imported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / "package.zip", "w") as archive:
                for path in (
                    "tmloader.exe",
                    "tmloader.ini",
                    "runtime/logs/debug.log",
                    "runtime/pub/home00/player.dat",
                ):
                    archive.writestr("TetraMaster/client/" + path, b"test")
                archive.writestr("TetraMaster/server/state.json", b"private")
            manifest = import_client(root / "package.zip", root / "output")
            self.assertFalse(manifest["has_originals"])
            self.assertEqual(
                {row["path"] for row in manifest["files"]},
                {"tmloader.exe", "tmloader.ini"},
            )

    def test_originals_and_pristine_template_are_imported_when_supplied(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            names = {
                "tmloader.exe",
                "tmloader.ini",
                "runtime/TM_native.dll",
                "runtime/polcore_native.dll",
                "runtime/data/gW201.dat",
                "runtime/data/utf/env.dat",
                "runtime/pub/home00/option.bin",
                "runtime/textures-hd/pack.json",
            }
            with zipfile.ZipFile(root / "package.zip", "w") as archive:
                for path in names:
                    archive.writestr("TetraMaster/client/" + path, b"test")
            manifest = import_client(root / "package.zip", root / "output")
            self.assertTrue(manifest["has_originals"])
            self.assertEqual({row["path"] for row in manifest["files"]}, names)

    def test_path_escape_rejected_before_archive_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / "package.zip", "w") as archive:
                archive.writestr("TetraMaster/client/../escape", b"bad")
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                import_client(root / "package.zip", root / "output")
            self.assertFalse((root / "output/portable-client.zip").exists())

    def test_drive_paths_and_windows_case_aliases_rejected(self):
        for names in (("runtime/data/C:escape",), ("tmloader.exe", "TMLOADER.EXE")):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                with zipfile.ZipFile(root / "package.zip", "w") as archive:
                    for name in names:
                        archive.writestr("TetraMaster/client/" + name, b"test")
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    import_client(root / "package.zip", root / "output")
                self.assertFalse((root / "output/portable-client.zip").exists())

    def test_dlls_alone_are_not_a_ready_to_play_import(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / "package.zip", "w") as archive:
                for name in (
                    "tmloader.exe",
                    "tmloader.ini",
                    "runtime/TM_native.dll",
                    "runtime/polcore_native.dll",
                ):
                    archive.writestr("TetraMaster/client/" + name, b"test")
            self.assertFalse(
                import_client(root / "package.zip", root / "output")["has_originals"]
            )

    def test_optional_server_import_preserves_data_but_excludes_runtime_state(self):
        names = {
            "tmserver/__init__.py",
            "tmserver/cli/serve.py",
            "examples/auto_rooms.example.json",
            "tmserver/data/economy_catalog.json",
            "tmserver/data/lobby_commands.tsv",
            "tmserver/data/profile_contract.json",
        }
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with zipfile.ZipFile(root / "package.zip", "w") as archive:
                for name in names | {
                    "state.json",
                    "logs/game.jsonl",
                    "tmserver/data/private.json",
                }:
                    archive.writestr("TetraMaster/server/" + name, b"fixture")
            self.assertEqual(
                import_server(root / "package.zip", root / "server"), len(names)
            )
            self.assertEqual(
                {
                    p.relative_to(root / "server").as_posix()
                    for p in (root / "server").rglob("*")
                    if p.is_file()
                },
                names,
            )


if __name__ == "__main__":
    unittest.main()
