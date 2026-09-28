"""Build and verify both client-only release archives from this checkout."""

import argparse
import hashlib
import json
import re
import tempfile
import zipfile
from pathlib import Path

from check_portable import check
from package_app import main as package

ROOT = Path(__file__).resolve().parents[1]


def build(loader, output, asset_dir):
    version = (ROOT / "version.txt").read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("version.txt must contain a numeric release version")
    report = {"version": version, "packages": {}}
    for full in (False, True):
        suffix = "-full" if full else ""
        archive = output / f"tmloader-{version}-windows-x86{suffix}.zip"
        arguments = ["--loader", str(loader), "--output", str(archive)]
        if full:
            arguments += [
                "--runtime",
                str(asset_dir / "runtime"),
                "--include-dlls",
                "--include-originals",
                "--viewer-defaults",
                str(asset_dir / "viewer-defaults/option.bin"),
                "--include-upgraded",
                str(asset_dir / "textures-hd/pack.json"),
            ]
        if package(arguments):
            raise RuntimeError(f"Could not build {archive.name}")
        with tempfile.TemporaryDirectory(prefix="verify-", dir=output) as directory:
            with zipfile.ZipFile(archive) as zipped:
                zipped.extractall(directory)
            verification = check(Path(directory) / "TetraMaster")
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        report["packages"][archive.name] = dict(
            verification, sha256=digest, bytes=archive.stat().st_size
        )
    (output / "SHA256SUMS.txt").write_text(
        "".join(
            f"{data['sha256']}  {name}\n" for name, data in report["packages"].items()
        ),
        encoding="utf-8",
        newline="\n",
    )
    (output / "validation.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "out/release")
    parser.add_argument("--asset-dir", type=Path, default=ROOT / "assets")
    args = parser.parse_args()
    build(args.loader, args.output, args.asset_dir)
