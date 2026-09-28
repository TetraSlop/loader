"""Verify an extracted client bundle without launching a game or any server."""

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


def tree_snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or path.is_junction():
            raise ValueError(f"Linked package path: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                result[path.relative_to(root).as_posix()] = hashlib.file_digest(
                    stream, "sha256"
                ).hexdigest()
    return result


def check(root):
    root = root.resolve(strict=True)
    before = tree_snapshot(root)
    receipt = json.loads((root / "package-manifest.json").read_text())
    if receipt.get("kind") != "client-only":
        raise ValueError("This is not a client-only bundle")
    if any(
        name.lower().startswith(("server/", "python/", "user/"))
        or name.lower().endswith((".py", ".pyc"))
        or name == "Start Server.cmd"
        for name in before
    ):
        raise ValueError("Server, Python or save files found in client bundle")
    expected = {row["path"]: row["sha256"] for row in receipt["files"]}
    if set(before) != set(expected) | {"package-manifest.json"}:
        raise ValueError(
            "Bundle inventory differs from its manifest; check a fresh extraction"
        )
    if any(before[name] != digest for name, digest in expected.items()):
        raise ValueError("Bundled bytes differ from the manifest")
    report = {
        "client_only": True,
        "files": len(before),
        "native_game_launched": False,
        "server_started": False,
    }
    try:
        if receipt["components"]["dlls"] and receipt["components"]["originals"]:
            if os.name != "nt":
                raise ValueError("Native setup verification requires Windows")
            result = subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(root / "app/client.ps1"),
                    "-Action",
                    "Check",
                ],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            if result.returncode:
                raise ValueError(result.stdout.strip() or result.stderr.strip())
            report["native_preflight"] = True
        else:
            report["native_preflight"] = "Requires importing original client files"
    finally:
        if tree_snapshot(root) != before:
            raise RuntimeError("Read-only check changed the extracted bundle")
    return dict(report, application_unchanged=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    try:
        print(json.dumps(check(parser.parse_args().root), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        raise SystemExit(str(exc)) from None
