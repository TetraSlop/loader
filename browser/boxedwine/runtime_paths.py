"""Generated runtimes and application archives stay outside tracked source."""

import os
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[2]


def runtime_root():
    return Path(
        os.environ.get("TM_BOXEDWINE_RUNTIME", PROJECT / "out/boxedwine")
    ).resolve()
