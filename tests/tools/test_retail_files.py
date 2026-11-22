"""retail-files.txt agrees with the loader, and prepare_runtime copies an install by it."""

import hashlib
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_runtime

# A miniature retail list: runtime path -> contents.
CONTENTS = {
    "TM_native.dll": b"game",
    "polcore_native.dll": b"viewer",
    "data/gW201.dat": b"art",
    "data/utf/env.dat": b"env",
    "pub/home00/option.bin": b"options",
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class RetailFilesTests(unittest.TestCase):
    def test_dll_hashes_match_the_pinned_builds(self):
        files = prepare_runtime.retail_files()
        source = (ROOT / "src/config/validation.cpp").read_text(encoding="utf-8")
        pinned = dict(re.findall(r'k(Game|Viewer)Hash\s*=\s*"([0-9A-F]{64})"', source))
        self.assertEqual(files["TM_native.dll"], pinned["Game"].lower())
        self.assertEqual(files["polcore_native.dll"], pinned["Viewer"].lower())

    def test_lists_the_files_the_loader_requires(self):
        files = prepare_runtime.retail_files()
        for name in ("data/gW201.dat", "data/utf/env.dat", "pub/home00/option.bin"):
            self.assertIn(name, files)
        self.assertFalse(any(name.startswith("textures/") for name in files))


class PrepareTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.files = {name: digest(data) for name, data in CONTENTS.items()}

    def write(self, path: str, data: bytes):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def install(self, game_dll=b"game", options=b"options"):
        self.write("game/TM.dll", game_dll)
        self.write("game/data/gW201.dat", b"art")
        self.write("viewer/viewer/com/polcore.dll", b"viewer")
        self.write("viewer/data/utf/env.dat", b"env")
        self.write("viewer/default/pub/home/option.bin", options)

    def prepare(self, source="game", viewer="viewer"):
        viewer_path = self.root / viewer if viewer else None
        return prepare_runtime.prepare(self.root / source, viewer_path, self.root / "runtime", self.files)

    def test_an_install_is_copied_into_the_runtime_layout(self):
        self.install()
        self.assertEqual(self.prepare(), [])
        for name, data in CONTENTS.items():
            self.assertEqual((self.root / "runtime" / name).read_bytes(), data)

    def test_a_runtime_folder_is_copied_as_is(self):
        for name, data in CONTENTS.items():
            self.write(f"old/{name}", data)
        self.assertEqual(self.prepare(source="old", viewer=None), [])

    def test_saved_viewer_settings_are_accepted(self):
        self.install(options=b"saved")
        self.assertEqual(self.prepare(), [])

    def test_another_game_build_is_rejected(self):
        self.install(game_dll=b"other")
        problems = self.prepare()
        self.assertEqual(len(problems), 1)
        self.assertIn("TM.dll", problems[0])

    def test_an_install_needs_the_viewer_folder(self):
        self.install()
        self.assertEqual(len(self.prepare(viewer=None)), 1)


if __name__ == "__main__":
    unittest.main()
