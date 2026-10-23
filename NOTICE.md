# Legal & Intellectual Property Notice

**NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.**

## Clean-Room Tooling Scope

This repository (`minecc`) contains **only original, MIT-licensed clean-room inspection and orchestration code**. It contains **zero** Minecraft binaries (`.jar`), **zero** compiled bytecode (`.class`), **zero** decompiled Minecraft `.java` sources, **zero** obfuscation mappings, and **zero** game assets (`assets/`, `data/`).

Minecraft: Java Edition — including its compiled client/server JARs, decompiled Java source code, mappings, textures, shaders, sounds, and data definitions — is proprietary software owned by **Mojang AB** / **Microsoft Corporation** and governed by the [Minecraft End User License Agreement (EULA)](https://www.minecraft.net/en-us/eula).

## Important Fact on GitHub & DMCA Compliance

Adding a text disclaimer (such as *"for educational purposes only"*, *"fair use"*, or *"all rights belong to Mojang"*) **does not prevent a valid DMCA takedown** under 17 U.S.C. § 512 if a repository actually commits or distributes Mojang's `client.jar`, `.class` bytecode, assets, or decompiled `.java` source code. Even though Mojang removed bytecode obfuscation starting in **Minecraft: Java Edition 26.1** (March 2026), the game remains closed-source commercial software, and Mojang explicitly prohibits redistributing game files or decompiled source trees.

The reason legitimate community tools (such as Fabric Loom, NeoForm, `mcsrc`, `MinecraftSourcesGenerator`, and `minecc`) operate cleanly on GitHub is **architectural, not a magic disclaimer**:

1. **Zero proprietary files in Git:** All local inspection caches and decompiled outputs write exclusively to `.local/` (git-ignored).
2. **Bring your own licensed installation:** You must own a legitimate copy of *Minecraft: Java Edition* installed via the official launcher (`~/.minecraft`, `%APPDATA%\.minecraft`, `~/Library/Application Support/minecraft`, Prism Launcher, or MultiMC) and accept the [Minecraft EULA](https://www.minecraft.net/en-us/eula) locally on your machine.
3. **Automated Git guardrails:** Run `git config core.hooksPath .githooks` (and `python3 -m minecc check-tree` in CI) so any accidental attempt to stage `*.jar`, `*.class`, `net/minecraft/**`, `com/mojang/**`, `assets/**`, or `data/**` is rejected automatically before commit or push.

## Third-Party Open-Source Acknowledgements

- **UI Theme & Explorer Workflow Inspiration (`ui/mcsrc.*`):**
  Adapted from [`FabricMC/mcsrc`](https://github.com/FabricMC/mcsrc) (`Copyright (c) 2025`, licensed under the MIT License). Rendered natively via the custom C++20 `HeliosUI` HTML5/CSS3/ES6 engine and software rasterizer without WebView2 or Chromium.
- **Embedded Bitmap Font Glyphs (`native/generated/font_atlas.hpp`):**
  Rasterized from **DejaVu Sans** and **DejaVu Sans Mono** (`Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.` / DejaVu Fonts contributors), distributed under the permissive Bitstream Vera / DejaVu Fonts license.

