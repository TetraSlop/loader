#!/usr/bin/env python3
"""Apply the LSB clang-format 22 rules to first-party C++ sources and tests."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def find_formatter(requested: str | None) -> str:
    candidates = [requested] if requested else [
        "clang-format-22",
        "clang-format",
        str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang-format.exe"),
    ]
    for candidate in candidates:
        executable = shutil.which(candidate)
        if not executable:
            continue
        result = subprocess.run(
            [executable, "--version"], capture_output=True, text=True, check=False, timeout=10
        )
        version = re.search(r"clang-format version (\d+)\.", result.stdout)
        if result.returncode == 0 and version and int(version[1]) == 22:
            return executable
    raise RuntimeError("clang-format 22 is required; install LLVM 22 or pass --clang-format PATH")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Report differences without editing files")
    parser.add_argument("--clang-format", metavar="PATH", help="Path to a clang-format 22 executable")
    args = parser.parse_args()
    executable = find_formatter(args.clang_format)
    # Only these owned source roots are eligible, never build trees or downloaded libraries.
    files = sorted(
        file
        for directory in (ROOT / "src", ROOT / "tests")
        for file in directory.rglob("*")
        if file.suffix in {".cpp", ".h", ".hpp"} and file.is_file() and not file.is_symlink()
    )
    if not files:
        raise RuntimeError("No first-party C++ files found")
    flags = ["--dry-run", "--Werror"] if args.check else ["-i"]
    failed = 0
    for file in files:
        result = subprocess.run(
            [executable, "--style=file", *flags, str(file)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            failed += 1
            print(f"FAIL {file.relative_to(ROOT)}", file=sys.stderr)
            print(result.stderr, file=sys.stderr, end="")
    action = "Checked" if args.check else "Formatted"
    print(f"{action} {len(files)} first-party C++ files with clang-format 22; {failed} failed.")
    return int(failed != 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
