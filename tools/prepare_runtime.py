"""Copy an existing user-owned runtime into this checkout's ignored runtime/ folder."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The native runtime setup requires Windows.")
    root = Path(__file__).resolve().parents[1]
    source = args.source.resolve(strict=True)
    runtime = root / "runtime"
    config = root / "tmloader.ini"
    loader = root / "build/RelWithDebInfo/tmloader.exe"
    if runtime.exists() or config.exists():
        parser.error(
            "runtime/ or tmloader.ini already exists; existing runtime/settings are never overwritten."
        )
    subprocess.run(
        [str(loader), "--runtime-dir", str(source), "--check"],
        check=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    runtime.mkdir()
    # Preserve the established low-integrity client's writable working directory.
    # Only this newly created, ignored copy is labelled; the source is read-only.
    subprocess.run(
        ["icacls", str(runtime), "/setintegritylevel", "(OI)(CI)L"],
        check=True,
        capture_output=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    for name in ("TM_native.dll", "polcore_native.dll"):
        shutil.copyfile(source / name, runtime / name)
    for name in ("data", "pub"):
        shutil.copytree(source / name, runtime / name, copy_function=shutil.copyfile)
    config.write_text(
        "runtime-dir=runtime\nserver=127.0.0.1\ngame-port=51241\n"
        "profile-port=51220\npol-id=EEEEEEEF\npassword=tm-local-test\n"
        "diagnostics=false\n",
        encoding="utf8",
    )
    subprocess.run(
        [str(loader), "--config", str(config), "--check"],
        check=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    print(f"Prepared {runtime}. No game launched.")


if __name__ == "__main__":
    main()
