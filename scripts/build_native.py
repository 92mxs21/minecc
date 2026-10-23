#!/usr/bin/env python3
"""Build all 4 native x86_64 executables (Linux ELF + Windows PE32+) cleanly via Zig C++ / g++."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN_DIR = ROOT / "bin"


def resolve_zig_cxx() -> list[str]:
    if shutil.which("zig"):
        return ["zig", "c++"]
    env = os.environ.copy()
    pytools = Path("/tmp/pytools")
    if pytools.exists():
        sys.path.insert(0, str(pytools))
    return [sys.executable, "-m", "ziglang", "c++"]


def run(cmd: list[str]) -> None:
    env = os.environ.copy()
    existing = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = f"/tmp/pytools:{existing}" if existing else "/tmp/pytools"
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


def main() -> int:
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    cxx = resolve_zig_cxx()
    common_flags = [
        "-std=c++20",
        "-O2",
        f"-I{ROOT}",
    ]

    # 1. Build standalone compiler (`minecc-uic`) for Linux x86_64 (glibc 2.28 baseline for universal Linux compatibility!)
    uic_linux = BIN_DIR / "minecc-uic-linux-x86_64"
    run(
        cxx
        + common_flags
        + [
            "-target",
            "x86_64-linux-gnu.2.28",
            "native/tools/minecc_uic.cpp",
            "-s",
            "-o",
            str(uic_linux),
        ]
    )

    # 2. Build standalone compiler (`minecc-uic.exe`) for Windows x86_64 (PE32+ static C++ runtime!)
    uic_win = BIN_DIR / "minecc-uic-windows-x86_64.exe"
    run(
        cxx
        + common_flags
        + [
            "-target",
            "x86_64-windows-gnu",
            "native/tools/minecc_uic.cpp",
            "-static",
            "-s",
            "-o",
            str(uic_win),
        ]
    )

    # 3. Run OUR NEWLY COMPILED `minecc-uic-linux-x86_64` compiler on `ui/mcsrc.html`!
    run(
        [
            str(uic_linux),
            "ui/mcsrc.html",
            "-o",
            "bin/mcsrc.muib",
            "--emit-cpp",
            "native/generated/mcsrc_bundle.hpp",
        ]
    )

    # 4. Build `minecc-studio` native IDE & software renderer for Linux x86_64
    studio_linux = BIN_DIR / "minecc-studio-linux-x86_64"
    run(
        cxx
        + common_flags
        + [
            "-target",
            "x86_64-linux-gnu.2.28",
            "native/app/studio_main.cpp",
            "-ldl",
            "-lpthread",
            "-s",
            "-o",
            str(studio_linux),
        ]
    )

    # 5. Build `minecc-studio.exe` native Win32 GUI IDE & software renderer for Windows x86_64
    studio_win = BIN_DIR / "minecc-studio-windows-x86_64.exe"
    run(
        cxx
        + common_flags
        + [
            "-target",
            "x86_64-windows-gnu",
            "native/app/studio_main.cpp",
            "-static",
            "-lgdi32",
            "-lcomdlg32",
            "-ldwmapi",
            "-lws2_32",
            "-luser32",
            "-Wl,--subsystem,windows",
            "-s",
            "-o",
            str(studio_win),
        ]
    )

    print("\nAll 4 native executables compiled & verified successfully:")
    for p in sorted(BIN_DIR.iterdir()):
        print(f"  - {p.relative_to(ROOT)}  ({p.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
