# minecc `v2.1.0`

**Pure C++20 Java 8–25 Decompiler, Version Diff, Hardware OpenGL Desktop Client, Headless AI CLI & WebAssembly Studio**

- **Live Web App (Desktop & Phone Responsive):** <https://92mxs21.github.io/minecc/>
- **Native PC Downloads (`Windows .exe` & `Linux x86_64`):** <https://github.com/92mxs21/minecc/releases/latest>

---

## Highlights in `v2.1.0`

- **Zero Lag Across Native & Web (`0.024 ms/frame`):**
  - **Native PC Client (`minecc-windows-x86_64.exe` / `minecc-linux-x86_64`):** Single-pass hardware OpenGL quad batch renderer (`opengl32.dll` + VSync on Windows, zero-backlog event-coalesced X11 on Linux) with an `8x`-supersampled `512x1024` vector icon & FreeType2 glyph atlas (`0.024 ms` CPU batch build at `1920x1080`) and a **100% lock-free multi-core C++20 worker thread pool** (`std::thread::hardware_concurrency()`).
  - **GitHub Pages Web App (`docs/minecc.wasm` — `384 KB`):** Compiles the exact C++20 ZIP64/DEFLATE + JVMS decompiler & version diff core directly to WebAssembly inside an off-main-thread `Web Worker` — instant native scrolling & clicking at `60/120/240 Hz` on desktop AND automatic responsive layout on phones (`100dvh`, touch drawers, mobile bottom navigation).
- **Clean Developer Home Screen:**
  - **Version Selection** + animated **`Show Snapshots`** toggle (`26.3`, `26.2`, `26.1`, `1.21.11`, `26.4-snapshot-5`, plus live Mojang manifest lookup and local `.minecraft/versions/` auto-detection).
  - **Diff Selection:** Compare any Base version / `.jar` against a Target version / `.jar` with `+Added` / `-Removed` / `~Modified` class filtering and unified line diffs.
  - **Platform-Tailored Decompiler Options:** Lightweight `WASM` optimization presets on the web app vs full multi-core `PC Native` thread-pool controls & whole-JAR parallel `.java` export on desktop.
- **Full Native C++20 Java 8–25 (`52.0–69.0`) Decompiler:**
  - Structured control flow (`if / else`, `while`, `tableswitch` / `lookupswitch`, `try / catch`), `invokedynamic` `StringConcatFactory` & `LambdaMetafactory` + synthetic `lambda$*` body inlining, JVMS §4.7.9.1 generic `Signature` recovery, `Record` components, `Enum` constants, `LocalVariableTable` names, automatic `import` folding, and Myers line diffing.
- **Startup Auto-Updater:**
  - Non-blocking background check against GitHub Releases on startup (`WinHTTP` on Windows, async check on Linux & Web) with smooth spring-animated notifications and 1-click update support.

---

## Headless CLI (`minecc ai`)

```bash
./bin/minecc-linux-x86_64 ai overview --jar path/to/client.jar --json
./bin/minecc-linux-x86_64 ai decompile net.minecraft.DetectedVersion --jar path/to/client.jar
./bin/minecc-linux-x86_64 ai diff net.minecraft.DetectedVersion --base old.jar --jar new.jar
./bin/minecc-linux-x86_64 ai export-all --jar path/to/client.jar --out .local/decompiled_src
./bin/minecc-linux-x86_64 ai xrefs DetectedVersion --json
```

---

## Building from Source

```bash
python3 -m pip install ziglang pillow
python3 scripts/build_native.py
PYTHONPATH=src python3 -m minecc check-tree
PYTHONPATH=src python3 -m unittest discover -s tests -v
```

> **Clean-Room Notice:** `minecc` contains zero proprietary Mojang source code, mappings, or game binaries. All decompilation runs locally in memory or inside your browser's WebAssembly sandbox against user-supplied archives.
