"""Build a portable Windows ZIP. Original client files are excluded by default."""

import argparse
import sys
from pathlib import Path

from distribution.assemble import assemble, write_archive

ROOT = Path(__file__).resolve().parents[1]


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--loader", type=Path, required=True, help="built x86 tmloader.exe"
    )
    parser.add_argument("--server", type=Path, default=ROOT.parent / "tmserver")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "out/dist/TetraMaster-portable.zip"
    )
    parser.add_argument("--cache", type=Path, default=ROOT / "out/package-cache")
    parser.add_argument(
        "--python-archive", type=Path, help="offline copy of the pinned Python ZIP"
    )
    parser.add_argument(
        "--runtime",
        type=Path,
        help="prepared runtime; read only with explicit include flags",
    )
    parser.add_argument("--include-dlls", action="store_true")
    parser.add_argument("--include-originals", action="store_true")
    parser.add_argument(
        "--viewer-defaults",
        type=Path,
        help="Viewer factory default/pub/home/option.bin",
    )
    parser.add_argument(
        "--include-upgraded",
        type=Path,
        metavar="PACK_JSON",
        help="include only this HD manifest's referenced images",
    )
    args = parser.parse_args(argv)
    if (args.include_dlls or args.include_originals) and not args.runtime:
        parser.error("--include-dlls/--include-originals require --runtime")
    if args.include_originals and not args.viewer_defaults:
        parser.error(
            "--include-originals requires --viewer-defaults (never a live profile)"
        )
    if args.output.exists():
        parser.error(f"Output already exists: {args.output}")
    return args


def main(argv=None):
    args = arguments(argv)
    try:
        files, manifest = assemble(args)
        write_archive(args.output, files)
    except (OSError, ValueError, KeyError) as exc:
        print(f"Package failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"Created {args.output.resolve()} ({args.output.stat().st_size / 1024 / 1024:.1f} MiB)"
    )
    print(f"Optional client files: {manifest['components']}")
    print("Existing saves, logs, local settings and source paths were not copied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
