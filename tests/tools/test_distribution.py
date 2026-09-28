"""Distribution boundaries: optional assets, private files and local setup safety."""

import argparse
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from distribution import app, assemble, assets


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
            cache=self.root,
            python_archive=None,
            server=self.root,
            loader=self.root / "loader",
            runtime=self.root / "private",
            include_dlls=False,
            include_originals=False,
            include_upgraded=None,
        )
        with (
            patch.object(
                assemble, "python_files", return_value={"python/python.exe": b"python"}
            ),
            patch.object(
                assemble,
                "server_files",
                return_value={"server/tmserver/__init__.py": b""},
            ),
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
        self.assertEqual(files["client/tmloader.ini"], app.config_text().encode())
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
                    cache=self.root,
                    python_archive=None,
                    server=self.root,
                    loader=self.root,
                    runtime=self.root,
                    viewer_defaults=self.root,
                    include_dlls=enabled == "dlls",
                    include_originals=enabled == "originals",
                    include_upgraded=self.root if enabled == "upgraded" else None,
                )
                with (
                    patch.object(assemble, "python_files", return_value={}),
                    patch.object(assemble, "server_files", return_value={}),
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

    def test_import_does_not_overwrite_existing_preferences_or_partially_copy(self):
        existing = self.file("client/runtime/pub/home00/option.bin", b"my preferences")
        args = argparse.Namespace(
            source=self.root,
            viewer_defaults=self.root,
            viewer_dir=None,
            texture_pack=None,
        )
        with (
            patch.object(
                assets, "installed_dlls", return_value={"TM_native.dll": b"dll"}
            ),
            patch.object(
                assets, "originals", return_value={"pub/home00/option.bin": b"factory"}
            ),
            patch.object(app, "prepare_writable_runtime") as permissions,
            self.assertRaisesRegex(ValueError, "Existing file differs"),
        ):
            app.import_files(self.root, args)
        self.assertEqual(existing.read_bytes(), b"my preferences")
        self.assertFalse((self.root / "client/runtime/TM_native.dll").exists())
        permissions.assert_not_called()

    def test_import_rejects_junction_destination_before_writes(self):
        args = argparse.Namespace(
            source=self.root,
            viewer_defaults=self.root,
            viewer_dir=None,
            texture_pack=None,
        )
        runtime = self.root / "client/runtime"
        with (
            patch.object(
                assets, "installed_dlls", return_value={"TM_native.dll": b"dll"}
            ),
            patch.object(assets, "originals", return_value={}),
            patch.object(
                Path,
                "is_junction",
                autospec=True,
                side_effect=lambda path: path == runtime,
            ),
            self.assertRaisesRegex(ValueError, "Linked paths"),
        ):
            app.import_files(self.root, args)
        self.assertFalse(runtime.exists())

    def test_offline_python_archive_checksum_is_enforced(self):
        archive = self.file("python.zip", b"not the official archive")
        with self.assertRaisesRegex(ValueError, "Unsupported or damaged"):
            assemble.python_files(self.root, archive)


if __name__ == "__main__":
    unittest.main()
