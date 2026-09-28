"""Assemble a Windows portable ZIP from explicit application components."""

import io
import json
import runpy
import struct
import subprocess
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from distribution import assets
from distribution.app import config_text

PYTHON_VERSION = "3.13.15"
PYTHON_ARCHIVE = f"python-{PYTHON_VERSION}-embed-amd64.zip"
PYTHON_URL = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/{PYTHON_ARCHIVE}"
PYTHON_SHA256 = "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf"


def python_files(cache, supplied=None):
    archive = supplied or cache / PYTHON_ARCHIVE
    if not archive.exists():
        if supplied:
            raise ValueError(f"Missing embedded Python archive: {supplied}")
        cache.mkdir(parents=True, exist_ok=True)
        print(f"Downloading Python {PYTHON_VERSION} from python.org...", flush=True)
        with urllib.request.urlopen(PYTHON_URL, timeout=60) as response:
            data = response.read(32 * 1024 * 1024)
        if assets.digest(data) != PYTHON_SHA256:
            raise ValueError("Embedded Python download checksum mismatch")
        archive.write_bytes(data)
    data = assets.verified_file(archive, PYTHON_SHA256)
    files = {}
    with zipfile.ZipFile(io.BytesIO(data)) as source:
        for info in source.infolist():
            if info.is_dir():
                continue
            name = assets.relative_name(info.filename)
            if f"python/{name}" in files:
                raise ValueError(f"Duplicate Python archive member: {name}")
            files[f"python/{name}"] = source.read(info)
    # No pip or global/user site packages. All imports stay in the extracted app.
    files["python/python313._pth"] = b"python313.zip\n.\n../server\n../app\n"
    return files


def loader_bytes(path):
    data = assets.regular_file(path).read_bytes()
    try:
        offset = struct.unpack_from("<I", data, 0x3C)[0]
        machine = struct.unpack_from("<H", data, offset + 4)[0]
        if (
            data[:2] != b"MZ"
            or data[offset : offset + 4] != b"PE\0\0"
            or machine != 0x14C
        ):
            raise ValueError("Loader must be a Windows x86 executable")
    except struct.error as exc:
        raise ValueError("Invalid loader executable") from exc
    return data


def server_files(source):
    # Reuse the server's exact runtime manifest rather than copying its working tree.
    manifest = runpy.run_path(str(source / "tools/package.py"))["runtime_manifest"](
        source
    )
    files = {
        f"server/{assets.relative_name(name)}": data for name, data in manifest.items()
    }
    name = "examples/auto_rooms.example.json"
    files[f"server/{name}"] = assets.regular_file(source / name).read_bytes()
    return files


def revision(source):
    result = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    # Per-file digests below are authoritative even if the checkout has local changes.
    return result.stdout.strip()


def application_files(loader_repo):
    files = {}
    for name in ("__init__.py", "assets.py", "app.py"):
        files[f"app/distribution/{name}"] = (Path(__file__).parent / name).read_bytes()
    for label, command in (
        ("Play", "play"),
        ("Check Setup", "check"),
        ("Import Game Files", "import"),
        ("Start Server", "server"),
    ):
        script = (
            '@echo off\r\nsetlocal\r\ncd /d "%~dp0"\r\n'
            f'"%~dp0python\\python.exe" -I -B -m distribution.app {command} %*\r\n'
            'set "app_exit=%ERRORLEVEL%"\r\n'
        )
        if command in ("check", "import"):
            script += "pause\r\n"
        else:
            script += 'if not "%app_exit%"=="0" pause\r\n'
        files[f"{label}.cmd"] = (script + "exit /b %app_exit%\r\n").encode()
    files["README.txt"] = (loader_repo / "docs/PORTABLE_README.txt").read_bytes()
    for path in sorted((loader_repo / "third_party").glob("*LICENSE*")):
        if not path.name.startswith("Catch2"):
            files[f"licenses/{path.name}"] = path.read_bytes()
    return files


def assemble(args):
    root = Path(__file__).resolve().parents[2]
    files = application_files(root) | python_files(args.cache, args.python_archive)
    files.update(server_files(args.server))
    files["client/tmloader.exe"] = loader_bytes(args.loader)
    files["client/tmloader.ini"] = config_text(bool(args.include_upgraded)).encode()
    selected = {}
    if args.include_dlls:
        selected.update(assets.dlls(args.runtime))
    if args.include_originals:
        selected.update(assets.originals(args.runtime, args.viewer_defaults))
    if args.include_upgraded:
        selected.update(assets.textures(args.include_upgraded))
    files.update({f"client/runtime/{name}": data for name, data in selected.items()})
    folded = [name.casefold() for name in files]
    if len(set(folded)) != len(folded):
        raise ValueError("Case-insensitive package path collision")
    manifest = {
        "format": 1,
        "platform": "windows-x64 (x86 game client)",
        "loader_revision": revision(root),
        "server_revision": revision(args.server),
        "python": {
            "version": PYTHON_VERSION,
            "url": PYTHON_URL,
            "sha256": PYTHON_SHA256,
        },
        "components": {
            "dlls": args.include_dlls,
            "originals": args.include_originals,
            "upgraded": bool(args.include_upgraded),
        },
        "files": [
            {"path": name, "bytes": len(data), "sha256": assets.digest(data)}
            for name, data in sorted(files.items())
        ],
    }
    files["package-manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return files, manifest


def write_archive(output, files):
    if output.exists():
        raise ValueError(f"Output already exists; choose a new filename: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-", dir=output.parent) as temporary:
        pending = Path(temporary) / "package.zip"
        with zipfile.ZipFile(
            pending, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6
        ) as archive:
            for name, data in sorted(files.items()):
                info = zipfile.ZipInfo(
                    f"TetraMaster/{assets.relative_name(name)}", (2020, 1, 1, 0, 0, 0)
                )
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                archive.writestr(info, data)
        # rename does not overwrite an existing target on Windows.
        pending.rename(output)
