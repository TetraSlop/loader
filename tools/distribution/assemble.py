"""Assemble a client-only Windows ZIP from explicit components."""

import json
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

from distribution import assets
from distribution.config import config_text


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


def revision(source):
    result = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def application_files(loader_repo):
    helpers = Path(__file__).parent / "windows"
    files = {
        f"app/{name}": (helpers / name).read_bytes()
        for name in ("client.ps1", "assets.ps1")
    }
    policy = {
        "dll_hashes": assets.DLL_HASHES,
        "default_options_hash": assets.DEFAULT_OPTIONS_HASH,
        "original_patterns": assets.ORIGINAL_PATTERNS,
    }
    files["app/asset-policy.json"] = (json.dumps(policy, indent=2) + "\n").encode()
    for label, action in (
        ("Play", "Play"),
        ("Check Setup", "Check"),
        ("Import Game Files", "Import"),
    ):
        # Execution policy applies only to this child process, not the machine/user settings.
        script = (
            "@echo off\r\nsetlocal\r\n"
            "powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
            f'-File "%~dp0app\\client.ps1" -Action {action} %*\r\n'
            'set "app_exit=%ERRORLEVEL%"\r\n'
        )
        script += (
            'if not "%app_exit%"=="0" pause\r\n' if action == "Play" else "pause\r\n"
        )
        files[f"{label}.cmd"] = (script + "exit /b %app_exit%\r\n").encode()
    files["Connection Settings.cmd"] = (
        b'@echo off\r\nnotepad.exe "%~dp0client\\tmloader.ini"\r\n'
    )
    files["README.txt"] = (loader_repo / "docs/PORTABLE_README.txt").read_bytes()
    for path in sorted((loader_repo / "third_party").glob("*LICENSE*")):
        if not path.name.startswith("Catch2"):
            files[f"licenses/{path.name}"] = path.read_bytes()
    return files


def assemble(args):
    root = Path(__file__).resolve().parents[2]
    files = application_files(root)
    files["client/tmloader.exe"] = loader_bytes(args.loader)
    files["client/tmloader.ini"] = config_text(
        bool(args.include_upgraded),
        args.server_address,
        args.game_port,
        args.profile_port,
    ).encode()
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
        "format": 2,
        "kind": "client-only",
        "platform": "windows (x86 game client)",
        "loader_revision": revision(root),
        "loader_version": (root / "version.txt").read_text().strip(),
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
        pending.rename(output)
