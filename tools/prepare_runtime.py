"""Build this checkout's ignored runtime/ folder from your own Tetra Master install and write tmloader.ini.

SOURCE is either the Tetra Master game folder (pass --viewer with the PlayOnline Viewer folder) or a folder
already laid out as a runtime. Only the files in retail-files.txt are copied, and each is checked against
its hash there.
"""

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

from build_release import ROOT, client_config

RETAIL_FILES = ROOT / "retail-files.txt"
# Changes once the Viewer saves settings, so a copy from a played install may differ.
VIEWER_SETTINGS = "pub/home00/option.bin"


def retail_files(path: Path = RETAIL_FILES) -> dict[str, str]:
    """Map each runtime path in a sha256sum-format list to its hash, skipping # comments."""
    files = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("#"):
            digest, name = line.split("  ", 1)
            files[name] = digest
    return files


def install_source(name: str, game: Path, viewer: Path) -> Path:
    """Where a runtime path comes from in a PlayOnline install."""
    if name == "TM_native.dll":
        return game / "TM.dll"
    if name == "polcore_native.dll":
        return viewer / "viewer/com/polcore.dll"
    if name == "data/utf/env.dat":
        return viewer / "data/utf/env.dat"
    if name == VIEWER_SETTINGS:
        return viewer / "default/pub/home/option.bin"
    return game / name


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(source: Path, viewer: Path | None, runtime: Path, files: dict[str, str]) -> list[str]:
    """Copy and check the retail files into runtime; return the problems found, empty on success."""
    laid_out = (source / "TM_native.dll").is_file()
    if not laid_out and viewer is None:
        return ["SOURCE is not a runtime folder, so --viewer must name the PlayOnline Viewer folder"]

    problems = []
    for name, expected in files.items():
        origin = source / name if laid_out else install_source(name, source, viewer)
        if not origin.is_file():
            problems.append(f"missing {origin}")
            continue
        if sha256(origin) != expected and name != VIEWER_SETTINGS:
            problems.append(f"unsupported or damaged {origin}")
            continue
        target = runtime / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="Tetra Master game folder, or a folder laid out as a runtime")
    parser.add_argument("--viewer", type=Path, help="PlayOnline Viewer folder (not needed for a runtime folder)")
    args = parser.parse_args()
    runtime, config = ROOT / "runtime", ROOT / "tmloader.ini"
    if runtime.exists() or config.exists():
        print("Error: runtime/ or tmloader.ini already exists; remove them to start fresh.", file=sys.stderr)
        return 1

    problems = prepare(args.source, args.viewer, runtime, retail_files())
    if problems:
        shutil.rmtree(runtime, ignore_errors=True)
        print("Error: " + "\n       ".join(problems), file=sys.stderr)
        return 1

    config.write_bytes(client_config(local=True))
    print(f"Prepared {runtime} and {config}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
