#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys


def resolve_python_executable(python_bin: str) -> pathlib.Path:
    if python_bin in {"python", "python3"}:
        virtual_env = os.environ.get("VIRTUAL_ENV")
        if virtual_env:
            venv_python = pathlib.Path(virtual_env) / "bin" / "python"
            if venv_python.exists():
                return pathlib.Path(os.path.abspath(venv_python))

    candidate = pathlib.Path(python_bin)
    if candidate.is_absolute() or candidate.parent != pathlib.Path():
        return pathlib.Path(os.path.abspath(candidate))

    resolved = shutil.which(python_bin)
    if resolved is None:
        raise FileNotFoundError(f"Python interpreter not found: {python_bin}")
    return pathlib.Path(os.path.abspath(resolved))


def main() -> int:
    parser = argparse.ArgumentParser(description="Remove the source-tree pygraphmini install from a Python environment.")
    parser.add_argument("--python", dest="python_bin", default=sys.executable,
                        help="Python interpreter to uninstall from. Default: current interpreter")
    args = parser.parse_args()

    python_exe = resolve_python_executable(args.python_bin)
    site_packages = pathlib.Path(
        subprocess.check_output(
            [str(python_exe), "-c", "import sysconfig; print(sysconfig.get_path('platlib'))"],
            text=True,
        ).strip()
    )

    removed_any = False
    for filename in ("graphmini_dev.pth", "graphmini_dev_bootstrap.py"):
        path = site_packages / filename
        if path.exists():
            path.unlink()
            print(f"Removed {path}")
            removed_any = True

    if not removed_any:
        print(f"Nothing to remove from {site_packages}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
