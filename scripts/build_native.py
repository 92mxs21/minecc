#!/usr/bin/env python3
"""Cross-platform C++20 & WebAssembly native build driver using Zig C++/Clang toolchain.

Builds all 5 clean-room production binaries (`0` external runtime dependencies):
  - `bin/minecc-uic-linux-x86_64`         (Linux x86_64 AOT HTML+CSS compiler)
  - `bin/minecc-uic-windows-x86_64.exe`   (Windows x86_64 AOT HTML+CSS compiler)
  - `docs/minecc.wasm`                    (WebAssembly C++20 Decompiler, Disassembler & Diff Core)
  - `bin/minecc-linux-x86_64`             (Linux x86_64 Native Desktop Studio + Headless AI CLI)
  - `bin/minecc-windows-x86_64.exe`       (Windows x86_64 Hardware OpenGL Studio + Auto-Updater + AI CLI)
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "bin"
DOCS = ROOT / "docs"


def zig_cxx(target: str, sources: list[Path], out: Path, opt: str = "-O3", extra: list[str] | None = None) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, "-m", "ziglang", "c++",
        "-std=c++20", opt, "-DNDEBUG",
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
    DOCS.mkdir(parents=True, exist_ok=True)

    print("==> Step 1/4: Building `minecc-uic` AOT Compiler (Linux + Windows)")
    zig_cxx(
        "x86_64-linux-gnu",
        [ROOT / "native/tools/minecc_uic.cpp"],
        BIN / "minecc-uic-linux-x86_64",
        extra=["-s"],
    )
    zig_cxx(
        "x86_64-windows-gnu",
        [ROOT / "native/tools/minecc_uic.cpp"],
        BIN / "minecc-uic-windows-x86_64.exe",
        extra=["-static", "-s", "-Wl,--stack,8388608"],
    )

    print("==> Step 2/4: Compiling `ui/flux_layout.html` + `ui/flux_theme.css` -> `native/generated/flux_ui_spec.hpp`")
    subprocess.run(
        [
            str(BIN / "minecc-uic-linux-x86_64"),
            str(ROOT / "ui/flux_layout.html"),
            "--css", str(ROOT / "ui/flux_theme.css"),
            "--emit-cpp", str(ROOT / "native/generated/flux_ui_spec.hpp"),
        ],
        check=True,
    )

    print("==> Step 3/4: Compiling C++20 WebAssembly Engine (`docs/minecc.wasm`)")
    zig_cxx(
        "wasm32-wasi",
        [ROOT / "native/wasm/minecc_wasm.cpp"],
        DOCS / "minecc.wasm",
        opt="-Os",
        extra=["-fno-exceptions", "-s", "-mexec-model=reactor", "-Wl,--export-dynamic"],
    )

    print("==> Step 4/4: Compiling Native GPU OpenGL Studio + Auto-Updater + AI CLI (Linux + Windows x86_64)")
    zig_cxx(
        "x86_64-linux-gnu",
        [ROOT / "native/app/studio_main.cpp"],
        BIN / "minecc-linux-x86_64",
        extra=["-ldl", "-pthread", "-s"],
    )
    zig_cxx(
        "x86_64-windows-gnu",
        [ROOT / "native/app/studio_main.cpp"],
        BIN / "minecc-windows-x86_64.exe",
        extra=[
            "-static",
            "-s",
            "-luser32",
            "-lgdi32",
            "-lcomdlg32",
            "-lshell32",
            "-ldwmapi",
            "-lws2_32",
            "-lopengl32",
            "-lwinhttp",
            "-Wl,--subsystem,windows",
            "-Wl,--stack,8388608",
        ],
    )
    print("==> All 5 native + WebAssembly targets built cleanly!")


if __name__ == "__main__":
    main()
