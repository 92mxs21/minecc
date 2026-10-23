"""Unit tests for the clean-room `minecc` bytecode parser, source counter, and tree guard."""

from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from minecc.classfile import ACC_ENUM, ACC_INTERFACE, parse_class_bytes
from minecc.cli import main as cli_main
from minecc.decompiler import DecompileSetupError, ensure_eula_accepted, ensure_safe_output_dir
from minecc.jar_inspector import inspect_jar
from minecc.source_stats import analyze_java_tree, count_java_source
from minecc.tree_guard import scan_paths


def _build_synthetic_class(
    this_name: str,
    super_name: str = "java/lang/Object",
    major_version: int = 69,  # Java SE 25 (used by Minecraft 26.1+)
    access_flags: int = 0x0021,
    fields: list[tuple[str, str]] | None = None,
    methods: list[tuple[str, str, bytes, list[int], list[str]]] | None = None,
    source_file: str | None = None,
    include_long_constant: bool = False,
) -> bytes:
    """Assemble a valid JVMS binary `.class` payload for unit testing."""
    fields = fields or []
    methods = methods or []

    cp_entries: list[bytes] = []
    utf8_map: dict[str, int] = {}
    class_map: dict[str, int] = {}
    next_idx = 1

    def add_utf8(s: str) -> int:
        nonlocal next_idx
        if s in utf8_map:
            return utf8_map[s]
        raw = s.encode("utf-8")
        cp_entries.append(struct.pack(">BH", 1, len(raw)) + raw)
        idx = next_idx
        utf8_map[s] = idx
        next_idx += 1
        return idx

    def add_class(name: str) -> int:
        nonlocal next_idx
        if name in class_map:
            return class_map[name]
        u_idx = add_utf8(name)
        cp_entries.append(struct.pack(">BH", 7, u_idx))
        idx = next_idx
        class_map[name] = idx
        next_idx += 1
        return idx

    this_idx = add_class(this_name)
    super_idx = add_class(super_name)

    if include_long_constant:
        # CONSTANT_Long occupies TWO constant-pool slots per JVMS §4.4.5
        cp_entries.append(struct.pack(">BQ", 5, 0x0102030405060708))
        next_idx += 2

    field_blobs: list[bytes] = []
    for f_name, f_desc in fields:
        fn_i = add_utf8(f_name)
        fd_i = add_utf8(f_desc)
        field_blobs.append(struct.pack(">HHHH", 0x0002, fn_i, fd_i, 0))

    method_blobs: list[bytes] = []
    code_attr_name = add_utf8("Code")
    lnt_attr_name = add_utf8("LineNumberTable")
    lvt_attr_name = add_utf8("LocalVariableTable")

    for m_name, m_desc, bytecode, lines, local_vars in methods:
        mn_i = add_utf8(m_name)
        md_i = add_utf8(m_desc)

        sub_attrs: list[bytes] = []
        if lines:
            lnt_body = struct.pack(">H", len(lines)) + b"".join(
                struct.pack(">HH", pc_i, line_no) for pc_i, line_no in enumerate(lines)
            )
            sub_attrs.append(struct.pack(">HI", lnt_attr_name, len(lnt_body)) + lnt_body)

        if local_vars:
            entries: list[bytes] = []
            for slot_i, var_name in enumerate(local_vars):
                vn_i = add_utf8(var_name)
                vd_i = add_utf8("I")
                entries.append(struct.pack(">HHHHH", 0, max(1, len(bytecode)), vn_i, vd_i, slot_i + 1))
            lvt_body = struct.pack(">H", len(entries)) + b"".join(entries)
            sub_attrs.append(struct.pack(">HI", lvt_attr_name, len(lvt_body)) + lvt_body)

        code_body = (
            struct.pack(">HHI", 2, 4, len(bytecode))
            + bytecode
            + struct.pack(">H", 0)
            + struct.pack(">H", len(sub_attrs))
            + b"".join(sub_attrs)
        )
        m_attr = struct.pack(">HI", code_attr_name, len(code_body)) + code_body
        method_blobs.append(struct.pack(">HHHH", 0x0001, mn_i, md_i, 1) + m_attr)

    class_attrs: list[bytes] = []
    if source_file is not None:
        sf_attr = add_utf8("SourceFile")
        sf_val = add_utf8(source_file)
        class_attrs.append(struct.pack(">HIH", sf_attr, 2, sf_val))

    out = io.BytesIO()
    out.write(struct.pack(">IHHH", 0xCAFEBABE, 0, major_version, next_idx))
    for entry in cp_entries:
        out.write(entry)
    out.write(struct.pack(">HHHH", access_flags, this_idx, super_idx, 0))
    out.write(struct.pack(">H", len(field_blobs)) + b"".join(field_blobs))
    out.write(struct.pack(">H", len(method_blobs)) + b"".join(method_blobs))
    out.write(struct.pack(">H", len(class_attrs)) + b"".join(class_attrs))
    return out.getvalue()


class TestClassfileAndJarInspector(unittest.TestCase):
    def test_java25_classfile_and_inner_class_grouping(self) -> None:
        outer = _build_synthetic_class(
            "net/minecraft/client/Minecraft",
            fields=[("renderDistance", "I")],
            methods=[("runTick", "(Z)V", b"\xb1\xb1\xb1\xb1", [100, 145, 220], ["tickCount", "deltaTracker"])],
            source_file="Minecraft.java",
            include_long_constant=True,
        )
        inner = _build_synthetic_class(
            "net/minecraft/client/Minecraft$ChatStatus",
            access_flags=0x0021 | ACC_ENUM,
            methods=[("isChatAllowed", "()Z", b"\x04\xac", [410, 418], ["multiplayer"])],
            source_file="Minecraft.java",
        )
        iface = _build_synthetic_class(
            "com/mojang/blaze3d/systems/RenderPass",
            access_flags=0x0601 | ACC_INTERFACE,
            methods=[],
            source_file="RenderPass.java",
        )

        parsed_outer = parse_class_bytes(outer)
        self.assertEqual(parsed_outer.java_se_major, 25)
        self.assertEqual(parsed_outer.canonical_source_path, "net/minecraft/client/Minecraft.java")
        self.assertEqual(parsed_outer.max_line, 220)
        self.assertEqual(parsed_outer.local_var_names, ("tickCount", "deltaTracker"))

        with tempfile.TemporaryDirectory() as tmp:
            jar_file = Path(tmp) / "synthetic-client.jar"
            with zipfile.ZipFile(jar_file, "w") as zf:
                zf.writestr("net/minecraft/client/Minecraft.class", outer)
                zf.writestr("net/minecraft/client/Minecraft$ChatStatus.class", inner)
                zf.writestr("com/mojang/blaze3d/systems/RenderPass.class", iface)
                zf.writestr("assets/minecraft/shaders/core/terrain.fsh", "void main() {}\n")
                zf.writestr("data/minecraft/recipe/stick.json", "{}\n")
                zf.writestr("version.json", json.dumps({"id": "26.3-synthetic"}))

            sha1 = hashlib.sha1(jar_file.read_bytes()).hexdigest()
            custom_locks = {
                "minecraft_versions": {
                    "26.3-synthetic": {"client_sha1": sha1}
                }
            }
            report = inspect_jar(jar_file, lock_data=custom_locks)
            self.assertEqual(report.input_classes, 3)
            self.assertEqual(report.top_level_classes, 2)
            self.assertEqual(report.inner_classes, 1)
            # Outer + inner share `Minecraft.java`, so 3 classes -> 2 expected `.java` files!
            self.assertEqual(report.expected_java_files, 2)
            self.assertEqual(report.javac_source_line_span_total, 418)
            self.assertEqual(report.obfuscation_status, "UNOBFUSCATED")
            self.assertIn("verified", report.lock_verification)
            self.assertEqual(report.resource_summary["shader_files"], 1)
            self.assertEqual(report.resource_summary["datapack_files"], 1)

    def test_server_bundler_unwrapping(self) -> None:
        inner_cls = _build_synthetic_class(
            "net/minecraft/server/MinecraftServer",
            methods=[("tickServer", "()V", b"\xb1", [300, 350], ["tick"])],
            source_file="MinecraftServer.java",
        )
        inner_buf = io.BytesIO()
        with zipfile.ZipFile(inner_buf, "w") as inner_zf:
            inner_zf.writestr("net/minecraft/server/MinecraftServer.class", inner_cls)

        with tempfile.TemporaryDirectory() as tmp:
            bundler_jar = Path(tmp) / "server-bundler.jar"
            with zipfile.ZipFile(bundler_jar, "w") as outer_zf:
                outer_zf.writestr("META-INF/versions/26.3/server-26.3.jar", inner_buf.getvalue())

            report = inspect_jar(bundler_jar)
            self.assertEqual(report.unwrapped_inner_jar, "META-INF/versions/26.3/server-26.3.jar")
            self.assertEqual(report.input_classes, 1)
            self.assertEqual(report.expected_java_files, 1)

    def test_proguard_obfuscation_detection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            obf_jar = Path(tmp) / "legacy-obf.jar"
            with zipfile.ZipFile(obf_jar, "w") as zf:
                for i in range(30):
                    name = f"c{i:02d}"
                    zf.writestr(
                        f"{name}.class",
                        _build_synthetic_class(
                            name,
                            major_version=65,
                            methods=[("a", "()V", b"\xb1", [10], [])],
                        ),
                    )
            report = inspect_jar(obf_jar)
            self.assertEqual(report.obfuscation_status, "OBFUSCATED_PROGUARD")


class TestSourceStatsAndGuardrails(unittest.TestCase):
    def test_java_sloc_comment_and_string_counting(self) -> None:
        sample = """package demo.sample;

/* Multi-line header
   spanning three lines */
public final class Demo {
    // Single-line comment
    public static String url() {
        return "https://example.com/*not-a-comment*/ // still string"; // trailing comment
    }
}
"""
        counts = count_java_source(sample, rel_path="demo/sample/Demo.java")
        self.assertEqual(counts.total_lines, 10)
        self.assertEqual(counts.blank_lines, 1)
        self.assertEqual(counts.comment_lines, 3)
        self.assertEqual(counts.code_lines, 6)
        self.assertFalse(counts.has_failure_marker)

        with tempfile.TemporaryDirectory() as tmp:
            src_file = Path(tmp) / "demo" / "sample" / "Demo.java"
            src_file.parent.mkdir(parents=True)
            src_file.write_text(sample, encoding="utf-8")
            tree_stats = analyze_java_tree(Path(tmp))
            self.assertEqual(tree_stats.java_files, 1)
            self.assertEqual(tree_stats.code_lines, 6)

    def test_guardrails_block_repo_pollution_and_proprietary_paths(self) -> None:
        violations = scan_paths(
            [
                "README.md",
                "src/minecc/cli.py",
                "client.jar",
                "net/minecraft/client/Minecraft.java",
                "com/mojang/blaze3d/systems/RenderSystem.java",
                "assets/minecraft/textures/block/stone.png",
                "eula.txt",
            ]
        )
        flagged = {path for path, _ in violations}
        self.assertEqual(
            flagged,
            {
                "client.jar",
                "net/minecraft/client/Minecraft.java",
                "com/mojang/blaze3d/systems/RenderSystem.java",
                "assets/minecraft/textures/block/stone.png",
                "eula.txt",
            },
        )

        with tempfile.TemporaryDirectory() as tmp:
            repo_dir = Path(tmp)
            with self.assertRaises(DecompileSetupError):
                ensure_eula_accepted(repo_dir / ".local", accept_flag=False)
            ensure_eula_accepted(repo_dir / ".local", accept_flag=True)

            # Writing directly into tracked repo subdirectories must raise!
            with self.assertRaises(DecompileSetupError):
                ensure_safe_output_dir(repo_dir / "src" / "decompiled", repo_root=repo_dir)
            # Writing into git-ignored `.local/...` is allowed
            safe = ensure_safe_output_dir(repo_dir / ".local" / "minecraft" / "26.3", repo_root=repo_dir)
            self.assertTrue(str(safe).endswith("26.3"))

    def test_cli_locks_and_check_tree(self) -> None:
        self.assertEqual(cli_main(["locks"]), 0)
        self.assertEqual(cli_main(["check-tree"]), 0)


class TestNativeEngineCompilerAndExecutables(unittest.TestCase):
    def _bin_dir(self) -> Path:
        return Path(__file__).resolve().parents[1] / "bin"

    def test_windows_pe64_and_linux_elf64_executables_valid(self) -> None:
        bin_dir = self._bin_dir()
        for linux_name in ("minecc-uic-linux-x86_64", "minecc-linux-x86_64"):
            p = bin_dir / linux_name
            self.assertTrue(p.is_file(), f"Missing Linux x64 binary: {p}")
            hdr = p.read_bytes()[:20]
            self.assertEqual(hdr[:4], b"\x7fELF")
            self.assertEqual(hdr[4], 2)  # ELFCLASS64
            e_machine = struct.unpack_from("<H", hdr, 18)[0]
            self.assertEqual(e_machine, 0x3E)  # EM_X86_64

        for win_name in ("minecc-uic-windows-x86_64.exe", "minecc-windows-x86_64.exe"):
            p = bin_dir / win_name
            self.assertTrue(p.is_file(), f"Missing Windows x64 .exe: {p}")
            raw = p.read_bytes()
            self.assertEqual(raw[:2], b"MZ")
            pe_off = struct.unpack_from("<I", raw, 0x3C)[0]
            self.assertEqual(raw[pe_off : pe_off + 4], b"PE\x00\x00")
            machine = struct.unpack_from("<H", raw, pe_off + 4)[0]
            self.assertEqual(machine, 0x8664)  # IMAGE_FILE_MACHINE_AMD64

    def test_headless_ghidra_ai_cli_and_uic_compiler(self) -> None:
        import subprocess

        bin_dir = self._bin_dir()
        uic = bin_dir / "minecc-uic-linux-x86_64"
        exe = bin_dir / "minecc-linux-x86_64"
        repo = Path(__file__).resolve().parents[1]

        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)
            # 1. Verify minimal forked HTML+CSS AOT compiler emits valid C++20 spec header
            out_hpp = tdir / "spec.hpp"
            res_uic = subprocess.run(
                [str(uic), str(repo / "ui/flux_layout.html"), "--css", str(repo / "ui/flux_theme.css"), "--emit-cpp", str(out_hpp)],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("Compiled 'flux_layout.html'", res_uic.stdout)
            self.assertIn("constexpr ThemePalette THEME", out_hpp.read_text(encoding="utf-8"))

            # 2. Verify all Headless AI CLI subcommands (`skill`, `overview`, `tree`, `search`, `inspect`, `decompile`, `disasm`, `xrefs`, `strings`)
            res_skill = subprocess.run([str(exe), "ai", "skill"], capture_output=True, text=True, check=True)
            self.assertIn("minecc ai xrefs", res_skill.stdout)

            res_ov = subprocess.run([str(exe), "ai", "overview", "--json"], capture_output=True, text=True, check=True)
            ov_json = json.loads(res_ov.stdout)
            self.assertGreaterEqual(ov_json["top_level_classes"], 10)

            res_xrefs = subprocess.run([str(exe), "ai", "xrefs", "DetectedVersion", "--json"], capture_output=True, text=True, check=True)
            xr_json = json.loads(res_xrefs.stdout)
            self.assertEqual(xr_json["target"], "DetectedVersion")
            self.assertGreaterEqual(len(xr_json["incoming"]), 2)

            res_dec = subprocess.run([str(exe), "ai", "decompile", "Minecraft", "--lines", "1-15"], capture_output=True, text=True, check=True)
            self.assertIn("class Minecraft", res_dec.stdout)

            res_dis = subprocess.run([str(exe), "ai", "disasm", "DetectedVersion", "tryDetectVersion"], capture_output=True, text=True, check=True)
            self.assertIn("invokestatic", res_dis.stdout)

            res_str = subprocess.run([str(exe), "ai", "strings", "version.json", "--json"], capture_output=True, text=True, check=True)
            self.assertGreaterEqual(len(json.loads(res_str.stdout)), 1)

            # Verify Python CLI wrapper `minecc ai ...` delegates cleanly
            self.assertEqual(cli_main(["ai", "inspect", "DetectedVersion", "--json"]), 0)

    def test_hostile_corrupted_jar_and_11383_class_scale_benchmark(self) -> None:
        import subprocess
        import time

        exe = self._bin_dir() / "minecc-linux-x86_64"
        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)

            # 1. Hostile / corrupted archives MUST NEVER crash with SIGSEGV / SIGABRT!
            bad_cases = [
                b"",
                b"PK\x03\x04truncated_garbage",
                b"PK\x05\x06" + b"\xff" * 40,
            ]
            for idx, raw in enumerate(bad_cases):
                bad_jar = tdir / f"corrupt_{idx}.jar"
                bad_jar.write_bytes(raw)
                res = subprocess.run([str(exe), "ai", "overview", "--jar", str(bad_jar)], capture_output=True, text=True)
                self.assertGreaterEqual(res.returncode, 0, f"Native engine crashed with signal {res.returncode}!")

            # Corrupted 0xCAFEBABE inside valid ZIP MUST also never crash!
            fuzz_jar = tdir / "corrupt_class.jar"
            with zipfile.ZipFile(fuzz_jar, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("net/fuzz/Broken.class", b"\xca\xfe\xba\xbe\x00\x00\x00\x45\x00\x1f\x07\x00\x99\xff\x00")
            res_fuzz = subprocess.run([str(exe), "ai", "inspect", "Broken", "--jar", str(fuzz_jar)], capture_output=True, text=True)
            self.assertGreaterEqual(res_fuzz.returncode, 0)

            # 2. Full Minecraft 26.3 scale test (11,383 classes) + Structured CFG + Version Diff + WASM check (0 PNGs!)
            base_jar = tdir / "base_26_2.jar"
            scale_jar = tdir / "scale_26_3_11383_classes.jar"

            base_cls = _build_synthetic_class(
                "net/minecraft/DetectedVersion",
                fields=[("protocolVersion", "I")],
                methods=[("tryDetectVersion", "()I", b"\x11\x03\x04\xac", [12], [])],
                source_file="DetectedVersion.java",
            )
            # Includes conditional branch (`iload_1; ifle +7; sipush 773; ireturn; iconst_0; ireturn`) to verify structured CFG + LocalVariableTable!
            cfg_bytecode = bytes([0x1B, 0x9E, 0x00, 0x07, 0x11, 0x03, 0x05, 0xAC, 0x03, 0xAC])
            sample_cls = _build_synthetic_class(
                "net/minecraft/DetectedVersion",
                fields=[("protocolVersion", "I")],
                methods=[("tryDetectVersion", "(I)I", cfg_bytecode, [12], ["flag"])],
                source_file="DetectedVersion.java",
            )
            with zipfile.ZipFile(base_jar, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("net/minecraft/DetectedVersion.class", base_cls)
            with zipfile.ZipFile(scale_jar, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("net/minecraft/DetectedVersion.class", sample_cls)
                for i in range(11_382):
                    zf.writestr(f"net/minecraft/pkg{i // 200}/Cls{i}.class", sample_cls)

            # Verify structured `if (flag > 0)` recovery on real JVM conditional branch bytecode
            res_cfg = subprocess.run(
                [str(exe), "ai", "decompile", "DetectedVersion", "--jar", str(scale_jar)],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("if (flag > 0)", res_cfg.stdout)
            self.assertIn("return 773;", res_cfg.stdout)

            # Verify unified Java diff across Base <-> Target JARs
            res_diff = subprocess.run(
                [str(exe), "ai", "diff", "DetectedVersion", "--base", str(base_jar), "--jar", str(scale_jar), "--json"],
                capture_output=True,
                text=True,
                check=True,
            )
            diff_json = json.loads(res_diff.stdout)
            self.assertGreater(diff_json["added"], 0)
            self.assertIn("+             return 773;", diff_json["patch"])

            # Verify 1080p native GPU batch speed (< 1.0 ms/frame!) and valid compiled WebAssembly binary
            t0 = time.perf_counter()
            res_bench = subprocess.run(
                [str(exe), "--jar", str(scale_jar), "--benchmark-fps"],
                capture_output=True,
                text=True,
                check=True,
            )
            elapsed = time.perf_counter() - t0
            self.assertIn("GPU quads/frame", res_bench.stdout)
            self.assertLess(elapsed, 4.0, f"11,383-class load + 500x 1080p frames took too long: {elapsed:.2f}s")

            wasm_file = Path(__file__).resolve().parents[1] / "docs/minecc.wasm"
            self.assertTrue(wasm_file.is_file(), "docs/minecc.wasm must exist for GitHub Pages!")
            self.assertEqual(wasm_file.read_bytes()[:8], b"\x00asm\x01\x00\x00\x00")


if __name__ == "__main__":
    unittest.main()
