"""Build and verify the minimal and full Windows client archives from this checkout."""

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ARCHIVE_ROOT = "TetraMaster"
# A fixed timestamp keeps archives reproducible.
TIMESTAMP = (2020, 1, 1, 0, 0, 0)


def loader_bytes(path: Path) -> bytes:
    """Return the loader if it is the expected x86 GUI executable with the legacy-compatible link flags."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    # The optional header follows the 4-byte signature and 20-byte file header.
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    subsystem, characteristics = struct.unpack_from("<HH", data, pe + 24 + 68)
    if data[:2] != b"MZ" or data[pe : pe + 4] != b"PE\0\0" or machine != 0x14C or magic != 0x10B:
        raise ValueError("Loader must be a Windows x86 (PE32) executable")
    if subsystem != 2:
        raise ValueError("Loader must use the GUI subsystem")
    # The game DLL runs legacy generated code; the loader links with /DYNAMICBASE:NO /NXCOMPAT:NO.
    if characteristics & (0x40 | 0x100):
        raise ValueError("Loader must be linked without ASLR and DEP")
    manifest = rb"requestedExecutionLevel\s+level=['\"]asInvoker['\"]\s+uiAccess=['\"]false['\"]"
    if not re.search(manifest, data) or b"requireAdministrator" in data:
        raise ValueError("Loader manifest must request asInvoker without uiAccess")
    return data


def client_config() -> bytes:
    return (
        b"# tmloader reads this file from its own folder. Command-line options override it.\n"
        b"# Replace the example server, ports and account with the ones from your server operator.\n"
        b"runtime-dir=runtime\nserver=127.0.0.1\ngame-port=51241\nprofile-port=51220\n"
        b"pol-id=EEEEEEEF\npassword=tm-local-test\n"
    )


def notices() -> bytes:
    """Third-party license texts for the libraries linked into the loader (Catch2 is test-only)."""
    sections = [
        f"==== {path.name} ====\n\n{path.read_text(encoding='utf-8').strip()}\n"
        for path in sorted((ROOT / "third_party").glob("*LICENSE*"))
        if not path.name.startswith("Catch2")
    ]
    return "\n".join(sections).encode()


def package_files(loader: bytes, full: bool) -> dict[str, bytes | Path]:
    """Map archive paths to their contents; large assets stay on disk until written."""
    files: dict[str, bytes | Path] = {
        "tmloader.exe": loader,
        "tmloader.ini": client_config(),
        "LICENSE.txt": ROOT / "LICENSE",
        "THIRD_PARTY_NOTICES.txt": notices(),
    }
    if full:
        runtime = ASSETS / "runtime"
        files.update(
            {f"runtime/{path.relative_to(runtime).as_posix()}": path for path in runtime.rglob("*") if path.is_file()}
        )
    return files


def write_archive(output: Path, files: dict[str, bytes | Path]) -> None:
    if output.exists():
        raise ValueError(f"Output already exists: {output}")
    partial = output.with_suffix(".partial")
    with zipfile.ZipFile(partial, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, source in sorted(files.items()):
            data = source.read_bytes() if isinstance(source, Path) else source
            archive.writestr(zipfile.ZipInfo(f"{ARCHIVE_ROOT}/{name}", TIMESTAMP), data, zipfile.ZIP_DEFLATED)
    partial.rename(output)


def tree_hashes(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            with path.open("rb") as stream:
                result[path.relative_to(root).as_posix()] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


def verify(archive: Path, full: bool) -> dict:
    """Extract the archive and, for the full variant, run the loader's read-only --check with its bundled config."""
    with tempfile.TemporaryDirectory(prefix="verify-", dir=archive.parent) as directory:
        with zipfile.ZipFile(archive) as zipped:
            zipped.extractall(directory)
        root = Path(directory) / ARCHIVE_ROOT
        before = tree_hashes(root)
        report = {"files": len(before), "native_preflight": "requires game files"}
        if full:
            # Piping output makes Windows wait for the GUI-subsystem loader.
            result = subprocess.run(
                [str(root / "tmloader.exe"), "--check"],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            if result.returncode:
                raise RuntimeError(f"Loader check failed: {result.stdout.strip() or result.stderr.strip()}")
            report["native_preflight"] = True
        if tree_hashes(root) != before:
            raise RuntimeError("The loader check changed the extracted package")
    return report


def build(loader_path: Path, output: Path) -> dict:
    version = (ROOT / "version.txt").read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("version.txt must contain a numeric release version")
    loader = loader_bytes(loader_path)
    output.mkdir(parents=True, exist_ok=True)
    report = {"version": version, "packages": {}}
    for variant in ("minimal", "full"):
        full = variant == "full"
        archive = output / f"tmloader-{version}-windows-x86{'-full' if full else ''}.zip"
        write_archive(archive, package_files(loader, full))
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        report["packages"][archive.name] = dict(verify(archive, full), sha256=digest, bytes=archive.stat().st_size)
    (output / "SHA256SUMS.txt").write_text(
        "".join(f"{data['sha256']}  {name}\n" for name, data in report["packages"].items()), newline="\n"
    )
    (output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, required=True, help="built x86 tmloader.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "out/release")
    args = parser.parse_args()
    try:
        report = build(args.loader, args.output)
    except (OSError, ValueError, RuntimeError, struct.error, subprocess.SubprocessError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
