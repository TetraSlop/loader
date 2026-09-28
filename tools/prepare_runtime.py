"""Copy the committed game assets into this checkout's ignored runtime/ folder and write tmloader.ini."""

import shutil
import sys

from build_release import ASSETS, ROOT, client_config


def main() -> int:
    runtime, config = ROOT / "runtime", ROOT / "tmloader.ini"
    if runtime.exists() or config.exists():
        print("Error: runtime/ or tmloader.ini already exists; remove them to start fresh.", file=sys.stderr)
        return 1
    shutil.copytree(ASSETS / "runtime", runtime)
    config.write_bytes(client_config())
    print(f"Prepared {runtime} and {config}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
