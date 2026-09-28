"""Distribution boundaries: optional assets, private files and local setup safety."""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from distribution import assemble, assets
from distribution.config import config_text


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

    def test_default_assembly_never_reads_optional_runtime(self):
        args = argparse.Namespace(
            server_address="203.0.113.10",
            game_port=52000,
            profile_port=52001,
            loader=self.root / "loader",
            runtime=self.root / "private",
            include_dlls=False,
            include_originals=False,
            include_upgraded=None,
        )
        with (
            patch.object(assemble, "loader_bytes", return_value=b"loader"),
            patch.object(assemble, "revision", return_value="revision"),
            patch.object(
                assets, "dlls", side_effect=AssertionError("read private DLL")
            ),
            patch.object(
                assets, "originals", side_effect=AssertionError("read original data")
            ),
            patch.object(
                assets, "textures", side_effect=AssertionError("read HD data")
            ),
        ):
            files, manifest = assemble.assemble(args)
        self.assertFalse(any(name.startswith("client/runtime/") for name in files))
        self.assertFalse(any(manifest["components"].values()))
        self.assertEqual(
            files["client/tmloader.ini"],
            config_text(False, "203.0.113.10", 52000, 52001).encode(),
        )
        self.assertEqual(manifest["kind"], "client-only")
        self.assertNotIn("server_revision", manifest)
        self.assertNotIn("python", manifest)
        self.assertFalse(
            any(
                name.startswith(("server/", "python/", "user/"))
                or name.endswith((".py", ".pyc"))
                for name in files
            )
        )
        self.assertNotIn("Start Server.cmd", files)
        self.assertIn("powershell.exe", files["Play.cmd"].decode())
        archive = self.root / "default.zip"
        assemble.write_archive(archive, files)
        with zipfile.ZipFile(archive) as zipped:
            for record in manifest["files"]:
                data = zipped.read("TetraMaster/" + record["path"])
                self.assertEqual(assets.digest(data), record["sha256"])
        with self.assertRaisesRegex(ValueError, "already exists"):
            assemble.write_archive(archive, {})

    def test_optional_components_are_independent(self):
        for enabled in ("dlls", "originals", "upgraded"):
            with self.subTest(enabled=enabled):
                args = argparse.Namespace(
                    server_address="127.0.0.1",
                    game_port=51241,
                    profile_port=51220,
                    loader=self.root,
                    runtime=self.root,
                    viewer_defaults=self.root,
                    include_dlls=enabled == "dlls",
                    include_originals=enabled == "originals",
                    include_upgraded=self.root if enabled == "upgraded" else None,
                )
                with (
                    patch.object(assemble, "loader_bytes", return_value=b"loader"),
                    patch.object(assemble, "revision", return_value="revision"),
                    patch.object(
                        assets, "dlls", return_value={"TM_native.dll": b"dll"}
                    ) as dll,
                    patch.object(
                        assets, "originals", return_value={"data/gW201.dat": b"art"}
                    ) as original,
                    patch.object(
                        assets, "textures", return_value={"textures-hd/a.png": b"hd"}
                    ) as hd,
                ):
                    _, manifest = assemble.assemble(args)
                self.assertEqual(dll.called, enabled == "dlls")
                self.assertEqual(original.called, enabled == "originals")
                self.assertEqual(hd.called, enabled == "upgraded")
                self.assertEqual(sum(manifest["components"].values()), 1)

    def test_original_selection_uses_factory_preferences_and_ignores_private_files(
        self,
    ):
        self.file("runtime/data/gW201.dat", b"art")
        self.file("runtime/data/utf/env.dat", b"env")
        self.file("runtime/data/secret.txt", b"private")
        self.file("runtime/pub/home00/option.bin", b"private")
        self.file("runtime/logs/game.log", b"private")
        defaults = self.file("viewer/default/pub/home/option.bin", b"factory")
        with patch.object(assets, "DEFAULT_OPTIONS_HASH", assets.digest(b"factory")):
            files = assets.originals(self.root / "runtime", defaults)
            self.assertEqual(
                set(files),
                {"data/gW201.dat", "data/utf/env.dat", "pub/home00/option.bin"},
            )
            self.assertEqual(files["pub/home00/option.bin"], b"factory")
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                assets.originals(
                    self.root / "runtime", self.root / "runtime/pub/home00/option.bin"
                )

    def texture_pack(self):
        image = self.file(
            "hd/images/card.png",
            b"\x89PNG\r\n\x1a\n"
            + struct.pack(">I4sII", 13, b"IHDR", 128, 128)
            + b"\0" * 9,
        )
        record = {
            "id": "card-1",
            "file": "images/card.png",
            "sha256": assets.digest(image.read_bytes()),
            "source_sha256": "a" * 64,
            "logical_size": [32, 32],
            "physical_size": [128, 128],
            "private_author_path": "C:/Users/someone/private",
        }
        manifest = self.file(
            "hd/pack.json",
            json.dumps(
                {
                    "version": 1,
                    "textures": [record],
                    "sources": [{"manifest": "C:/private/artwork"}],
                }
            ).encode(),
        )
        self.file("hd/unreferenced.png", b"private")
        return manifest, record

    def test_texture_export_strips_source_paths_and_unreferenced_files(self):
        manifest, _ = self.texture_pack()
        files = assets.textures(manifest)
        self.assertEqual(
            set(files), {"textures-hd/images/card.png", "textures-hd/pack.json"}
        )
        self.assertNotIn(b"private", files["textures-hd/pack.json"])

    def test_texture_traversal_and_bad_hash_are_rejected(self):
        manifest, record = self.texture_pack()
        for name in (
            "../outside.png",
            "C:/outside.png",
            "/outside.png",
            "images\\card.png",
        ):
            record["file"] = name
            manifest.write_text(json.dumps({"version": 1, "textures": [record]}))
            with self.subTest(name=name), self.assertRaises(ValueError):
                assets.textures(manifest)
        record["file"], record["sha256"] = "images/card.png", "0" * 64
        manifest.write_text(json.dumps({"version": 1, "textures": [record]}))
        with self.assertRaisesRegex(ValueError, "damaged"):
            assets.textures(manifest)

    def test_endpoint_validation(self):
        for address, game, profile in [
            ("example.com", 51241, 51220),
            ("127.0.0.1", 0, 51220),
            ("127.0.0.1", 51241, 51241),
        ]:
            with (
                self.subTest(address=address, game=game, profile=profile),
                self.assertRaises(ValueError),
            ):
                config_text(False, address, game, profile)

    @unittest.skipUnless(os.name == "nt", "Windows client importer")
    def test_import_rejects_private_defaults_and_preserves_existing_preferences(self):
        helpers = Path(assemble.__file__).parent / "windows"
        for name in ("client.ps1", "assets.ps1"):
            self.file(f"app/{name}", (helpers / name).read_bytes())
        self.file(
            "app/asset-policy.json",
            json.dumps(
                {
                    "dll_hashes": dict.fromkeys(
                        assets.DLL_HASHES, assets.digest(b"dll")
                    ),
                    "default_options_hash": assets.digest(b"factory"),
                    "original_patterns": assets.ORIGINAL_PATTERNS,
                }
            ).encode(),
        )
        self.file("client/tmloader.ini", config_text().encode())
        for name in assets.DLL_HASHES:
            self.file(f"source/{name}", b"dll")
        self.file("source/data/gW201.dat", b"art")
        self.file("source/data/utf/env.dat", b"env")
        defaults = self.file("factory/option.bin", b"private")
        for expected in ("Unsupported or damaged", "Existing file differs"):
            with self.subTest(expected=expected):
                before = {
                    path: path.read_bytes()
                    for path in (self.root / "client").rglob("*")
                    if path.is_file()
                }
                result = subprocess.run(
                    [
                        "powershell.exe",
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        str(self.root / "app/client.ps1"),
                        "-Action",
                        "Import",
                        "-Source",
                        str(self.root / "source"),
                        "-ViewerDefaults",
                        str(defaults),
                    ],
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertEqual(
                    before,
                    {
                        path: path.read_bytes()
                        for path in (self.root / "client").rglob("*")
                        if path.is_file()
                    },
                )
                self.assertFalse(list((self.root / "client").glob(".import-*")))
            defaults.write_bytes(b"factory")
            self.file("client/runtime/pub/home00/option.bin", b"personal preferences")


if __name__ == "__main__":
    unittest.main()
