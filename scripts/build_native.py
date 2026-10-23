#!/usr/bin/env python3
"""Cross-platform C++20 native build driver using Zig C++/Clang toolchain.

Builds into `bin/` (persisted across snapshots, Git, and GitHub Releases):
  - `bin/minecc-uic-linux-x86_64`         (Linux x86_64 AOT HTML+CSS compiler)
  - `bin/minecc-uic-windows-x86_64.exe`   (Windows x86_64 AOT HTML+CSS compiler)
  - `bin/minecc-linux-x86_64`             (Linux x86_64 Native Dark Studio + Headless AI CLI)
  - `bin/minecc-windows-x86_64.exe`       (Windows x86_64 Native Win32 Dark Studio + Headless AI CLI)
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "bin"


def zig_cxx(target: str, sources: list[Path], out: Path, extra: list[str] | None = None) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, "-m", "ziglang", "c++",
        "-std=c++20", "-O3", "-DNDEBUG",
        "-target", target,
        f"-I{ROOT}",
        *[str(s) for s in sources],
        "-o", str(out),
        *(extra or []),
    ]
    print(f"  [zig c++] {target:20s} -> {out.relative_to(ROOT)}")
    subprocess.run(cmd, check=True)


def main() -> None:
    if BIN.exists():
        shutil.rmtree(BIN)
    BIN.mkdir(parents=True, exist_ok=True)

    print("==> Step 1/3: Building `minecc-uic` v2.0 Minimal HTML+CSS AOT Compiler (Linux + Windows)")
    zig_cxx(
        "x86_64-linux-gnu",
        [ROOT / "native/tools/minecc_uic.cpp"],
        BIN / "minecc-uic-linux-x86_64",
        ["-s"],
    )
    zig_cxx(
        "x86_64-windows-gnu",
        [ROOT / "native/tools/minecc_uic.cpp"],
        BIN / "minecc-uic-windows-x86_64.exe",
        ["-static", "-s", "-Wl,--stack,8388608"],
    )

    print("==> Step 2/3: Compiling `ui/flux_layout.html` + `ui/flux_theme.css` -> `native/generated/flux_ui_spec.hpp`")
    subprocess.run(
        [
            str(BIN / "minecc-uic-linux-x86_64"),
            str(ROOT / "ui/flux_layout.html"),
            "--css", str(ROOT / "ui/flux_theme.css"),
            "--emit-cpp", str(ROOT / "native/generated/flux_ui_spec.hpp"),
        ],
        check=True,
    )

    print("==> Step 3/3: Compiling Native Self-Rendered Studio + Headless AI CLI (Linux + Windows x86_64)")
    zig_cxx(
        "x86_64-linux-gnu",
        [ROOT / "native/app/studio_main.cpp"],
        BIN / "minecc-linux-x86_64",
        ["-ldl", "-pthread", "-s"],
    )
    zig_cxx(
        "x86_64-windows-gnu",
        [ROOT / "native/app/studio_main.cpp"],
        BIN / "minecc-windows-x86_64.exe",
        [
            "-static",
            "-s",
            "-luser32",
            "-lgdi32",
            "-lcomdlg32",
            "-ldwmapi",
            "-lws2_32",
            "-Wl,--subsystem,windows",
            "-Wl,--stack,8388608",
        ],
    )
    print("==> Native v2.0 builds succeeded cleanly in `bin/`!")


if __name__ == "__main__":
    main()
