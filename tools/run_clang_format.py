"""Apply or check clang-format 22 on first-party C++ sources and tests."""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_MAJOR = 22


def find_formatter(requested: str | None) -> str:
    candidates = (
        [requested]
        if requested
        else [
            f"clang-format-{REQUIRED_MAJOR}",
            "clang-format",
            str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang-format.exe"),
        ]
    )
    for candidate in candidates:
        executable = shutil.which(candidate)
        if not executable:
            continue
        result = subprocess.run([executable, "--version"], capture_output=True, text=True, check=False, timeout=10)
        version = re.search(r"clang-format version (\d+)\.", result.stdout)
        if result.returncode == 0 and version and int(version[1]) == REQUIRED_MAJOR:
            return executable
    raise RuntimeError(f"clang-format {REQUIRED_MAJOR} is required; install LLVM or pass --clang-format PATH")


def format_sources(executable: str, check: bool) -> int:
    """Return clang-format's exit status; with --check it lists every misformatted line."""
    files = sorted(
        file
        for directory in (ROOT / "src", ROOT / "tests")
        for file in directory.rglob("*")
        if file.suffix in {".cpp", ".h", ".hpp"} and file.is_file() and not file.is_symlink()
    )
    if not files:
        raise RuntimeError("No first-party C++ files found")
    flags = ["--dry-run", "--Werror"] if check else ["-i"]
    names = [file.relative_to(ROOT).as_posix() for file in files]
    result = subprocess.run([executable, "--style=file", *flags, *names], cwd=ROOT, check=False)
    action = "Checked" if check else "Formatted"
    print(f"{action} {len(files)} first-party C++ files with clang-format {REQUIRED_MAJOR}.")
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report differences without editing files")
    parser.add_argument("--clang-format", metavar="PATH", help=f"path to a clang-format {REQUIRED_MAJOR} executable")
    args = parser.parse_args()
    try:
        failed = format_sources(find_formatter(args.clang_format), args.check)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
