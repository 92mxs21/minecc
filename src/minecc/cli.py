"""Command-line interface for `minecc`."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from minecc.decompiler import DecompileSetupError, run_local_decompile
from minecc.jar_inspector import find_local_version_jar, inspect_jar, load_research_lock
from minecc.source_stats import analyze_java_tree
from minecc.tree_guard import check_git_repository


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _cmd_locks(args: argparse.Namespace) -> int:
    lock_data = load_research_lock(_repo_root())
    if args.json:
        print(json.dumps(lock_data, indent=2))
        return 0

    print("Pinned Reference Builds & Audited Vanilla Metrics (research-lock.json)")
    print("=" * 74)
    for ver, info in lock_data.get("minecraft_versions", {}).items():
        obf_tag = "obfuscated (requires remapping)" if info.get("obfuscated") else "unobfuscated (direct Vineflower)"
        print(
            f"\nMinecraft {ver} — {info.get('release_name', '')}\n"
            f"  Released:        {info.get('release_date')} ({info.get('release_type')})\n"
            f"  Java runtime:    Java SE {info.get('java_major')} (classfile {info.get('classfile_major')}.0)\n"
            f"  Bytecode names:  {obf_tag}"
        )
        if "client_sha1" in info:
            print(f"  client.jar SHA1: {info['client_sha1']} (~{info.get('client_approx_mib', '?')} MiB)")
        if "audited_metrics" in info:
            audit = info["audited_metrics"]
            c = audit["client"]
            s = audit["dedicated_server_inner"]
            print(f"  Verified with:   {audit['verified_with']} ({audit['verified_date']})")
            print(
                f"  Client JAR:      {c['input_classes']:,} .class files | "
                f"{c['input_methods']:,} methods | {c['emitted_java_files']:,} .java files"
            )
            print(
                f"  Server inner:    {s['input_classes']:,} .class files | "
                f"{s['input_methods']:,} methods | {s['emitted_java_files']:,} .java files"
            )
    vf = lock_data.get("vineflower", {})
    if vf:
        print(f"\nPinned Decompiler: Vineflower {vf['version']} ({vf['license']})")
        print(f"  SHA-256: {vf['sha256']}")
    return 0


def _resolve_jar_args(args: argparse.Namespace) -> tuple[Path, Path | None]:
    if args.jar:
        jar = Path(args.jar).expanduser()
        if not jar.is_file():
            raise FileNotFoundError(f"JAR file not found: {jar}")
        vjson = Path(args.version_json).expanduser() if getattr(args, "version_json", None) else None
        return jar, vjson
    mc_dir = Path(args.minecraft_dir).expanduser() if args.minecraft_dir else None
    return find_local_version_jar(args.version, minecraft_dir=mc_dir)


def _cmd_inspect(args: argparse.Namespace) -> int:
    try:
        jar_path, _ = _resolve_jar_args(args)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    report = inspect_jar(jar_path, version_hint=args.version, lock_data=load_research_lock(_repo_root()))
    if args.json:
        print(json.dumps(report.to_dict(), indent=2))
        return 0

    print(f"JAR Inspection Report: {report.jar_path}")
    print("=" * 74)
    print(f"Detected version:       {report.detected_version or 'unknown'}")
    print(f"Archive size:           {report.file_size_bytes:,} bytes ({report.file_size_bytes / (1024 * 1024):.2f} MiB)")
    print(f"SHA-1:                  {report.sha1}")
    print(f"Lock verification:      {report.lock_verification}")
    if report.unwrapped_inner_jar:
        print(f"Bundled inner JAR:      {report.unwrapped_inner_jar} (SHA-256 {report.inner_jar_sha256})")
    print(f"Obfuscation check:      {report.obfuscation_status}")
    print(f"                        {report.obfuscation_reason}")
    print(f"Java classfile targets: {report.java_se_versions}")
    print("-" * 74)
    print(f"Compiled .class files:  {report.input_classes:,} ({report.top_level_classes:,} top-level + {report.inner_classes:,} inner/$)")
    print(f"Expected .java files:   {report.expected_java_files:,}")
    print(f"Methods / Fields:       {report.total_methods:,} methods | {report.total_fields:,} fields")
    print(f"JVM bytecode payload:   {report.total_bytecode_bytes:,} bytes")
    print(f"javac source line span: {report.javac_source_line_span_total:,} lines (across SourceFile max lines)")
    print(f"javac executable lines: {report.javac_executable_lines_total:,} bytecode-mapped source lines")
    print(f"Class kinds:            {report.class_kinds}")
    print(f"Namespaces:             {report.namespace_class_counts}")
    print(f"Bundled resources:      {report.resource_summary}")
    return 0


def _cmd_decompile(args: argparse.Namespace) -> int:
    try:
        jar_path, vjson = _resolve_jar_args(args)
        result = run_local_decompile(
            jar_path=jar_path,
            repo_root=_repo_root(),
            version_hint=args.version,
            version_json=vjson,
            minecraft_dir=Path(args.minecraft_dir) if args.minecraft_dir else None,
            out_dir=Path(args.out) if args.out else None,
            accept_eula=args.accept_eula,
            threads=args.threads,
            heap=args.heap,
        )
    except (FileNotFoundError, DecompileSetupError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    cov = result["coverage"]
    sm = result["decompiled_source_metrics"]
    print(f"Local Decompilation Complete ({result['version']})")
    print("=" * 74)
    print(f"Output directory (git-ignored): {result['output_directory']}")
    print(f"Saved JSON audit report:        {result['saved_report']}")
    print(f"Emitted .java files:            {cov['emitted_java_files']:,} / {cov['expected_java_files']:,} expected")
    print(f"Total lines (with blanks/docs): {sm['total_lines']:,}")
    print(f"Source lines of code (SLOC):    {sm['code_lines_sloc']:,}")
    print(f"Comment / Blank lines:          {sm['comment_lines']:,} comments | {sm['blank_lines']:,} blank")
    print(f"Failed-decompile markers:       {cov['files_with_failure_markers']:,}")
    return 0


def _cmd_source_stats(args: argparse.Namespace) -> int:
    target = Path(args.dir).expanduser()
    if not target.is_dir():
        print(f"error: directory not found: {target}", file=sys.stderr)
        return 2
    stats = analyze_java_tree(target)
    if args.json:
        print(json.dumps(stats.to_dict(), indent=2))
        return 0
    print(f"Source Metrics: {stats.root_dir}")
    print("=" * 74)
    print(f"Java files:              {stats.java_files:,}")
    print(f"Total bytes:             {stats.total_bytes:,}")
    print(f"Total lines:             {stats.total_lines:,}")
    print(f"Code lines (SLOC):       {stats.code_lines:,}")
    print(f"Comment lines:           {stats.comment_lines:,}")
    print(f"Blank lines:             {stats.blank_lines:,}")
    print(f"Failure-marker files:    {stats.files_with_failure_markers:,}")
    return 0


def _cmd_check_tree(args: argparse.Namespace) -> int:
    root = Path(args.repo).expanduser() if args.repo else _repo_root()
    violations = check_git_repository(root)
    if violations:
        print("PUBLIC TREE CHECK FAILED — disallowed proprietary/artifact paths detected:", file=sys.stderr)
        for path, reason in violations:
            print(f"  - {path}: {reason}", file=sys.stderr)
        return 1
    print("OK: Public Git tree contains zero proprietary Minecraft files, JARs, or bytecode.")
    return 0


def _native_studio_exe() -> Path:
    exe_name = "minecc-windows-x86_64.exe" if os.name == "nt" else "minecc-linux-x86_64"
    return _repo_root() / "bin" / exe_name


def _cmd_uic(args: argparse.Namespace) -> int:
    exe_name = "minecc-uic-windows-x86_64.exe" if os.name == "nt" else "minecc-uic-linux-x86_64"
    exe = _repo_root() / "bin" / exe_name
    if not exe.exists():
        print(f"error: native compiler binary not found at {exe}", file=sys.stderr)
        return 2
    return subprocess.call([str(exe), *args.uic_args])


def _cmd_studio(args: argparse.Namespace) -> int:
    exe = _native_studio_exe()
    if not exe.exists():
        print(f"error: native studio binary not found at {exe}", file=sys.stderr)
        return 2
    return subprocess.call([str(exe), *args.studio_args])


def _cmd_ai(args: argparse.Namespace) -> int:
    exe = _native_studio_exe()
    if not exe.exists():
        print(f"error: native AI engine binary not found at {exe}", file=sys.stderr)
        return 2
    return subprocess.call([str(exe), "ai", *args.ai_args])


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="minecc",
        description=(
            "Clean-room local JVM bytecode inspector and Vineflower decompilation runner "
            "for locally installed copies of Minecraft: Java Edition."
        ),
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_locks = sub.add_parser("locks", help="Display pinned checksums and audited vanilla 26.3 metrics")
    p_locks.add_argument("--json", action="store_true", help="Emit JSON output")
    p_locks.set_defaults(func=_cmd_locks)

    p_inspect = sub.add_parser("inspect", help="Inspect a local Minecraft JAR without decompiling")
    p_inspect.add_argument("--version", default="26.3", help="Launcher version ID (default: 26.3)")
    p_inspect.add_argument("--jar", help="Explicit path to local client.jar / server.jar")
    p_inspect.add_argument("--minecraft-dir", help="Custom .minecraft root directory")
    p_inspect.add_argument("--json", action="store_true", help="Emit JSON output")
    p_inspect.set_defaults(func=_cmd_inspect)

    p_decomp = sub.add_parser(
        "decompile",
        help="Decompile your local Minecraft 26.1+ JAR into git-ignored .local/ with Vineflower 1.12.0",
    )
    p_decomp.add_argument("--version", default="26.3", help="Launcher version ID (default: 26.3)")
    p_decomp.add_argument("--jar", help="Explicit path to local client.jar / server.jar")
    p_decomp.add_argument("--version-json", help="Optional path to <version>.json for library resolution")
    p_decomp.add_argument("--minecraft-dir", help="Custom .minecraft root directory")
    p_decomp.add_argument("--out", help="Output directory (must be inside .local/ or outside this repo)")
    p_decomp.add_argument("--accept-eula", action="store_true", help="Acknowledge the Minecraft EULA locally")
    p_decomp.add_argument("--threads", type=int, help="Vineflower worker thread count")
    p_decomp.add_argument("--heap", default="4G", help="JVM max heap size for Vineflower (default: 4G)")
    p_decomp.add_argument("--json", action="store_true", help="Emit JSON output")
    p_decomp.set_defaults(func=_cmd_decompile)

    p_stats = sub.add_parser("source-stats", help="Count exact SLOC, comments, blanks, and subsystems in a .java folder")
    p_stats.add_argument("dir", help="Directory containing decompiled .java sources")
    p_stats.add_argument("--json", action="store_true", help="Emit JSON output")
    p_stats.set_defaults(func=_cmd_source_stats)

    p_guard = sub.add_parser("check-tree", help="Verify zero proprietary game files are tracked or staged in Git")
    p_guard.add_argument("--repo", help="Repository root path (defaults to minecc checkout)")
    p_guard.set_defaults(func=_cmd_check_tree)

    p_uic = sub.add_parser("uic", help="Invoke native C++20 minimal HTML+CSS AOT compiler (bin/minecc-uic-*)")
    p_uic.add_argument("uic_args", nargs=argparse.REMAINDER, help="Arguments forwarded to minecc-uic")
    p_uic.set_defaults(func=_cmd_uic)

    p_studio = sub.add_parser("studio", help="Launch native C++20 dark studio or live server (bin/minecc-*)")
    p_studio.add_argument("studio_args", nargs=argparse.REMAINDER, help="Arguments forwarded to native studio")
    p_studio.set_defaults(func=_cmd_studio)

    p_ai = sub.add_parser("ai", help="Headless Ghidra-style AI reverse-engineering CLI (zero MCP / zero JVM)")
    p_ai.add_argument("ai_args", nargs=argparse.REMAINDER, help="AI CLI subcommand & flags")
    p_ai.set_defaults(func=_cmd_ai)

    return parser


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    # Forward raw tail directly for `ai`, `studio`, and `uic` so flags like `--json` or `--help` pass straight through!
    if raw and raw[0] in ("ai", "studio", "uic"):
        head, tail = raw[0], raw[1:]
        ns = argparse.Namespace(ai_args=tail, studio_args=tail, uic_args=tail)
        if head == "ai":
            return _cmd_ai(ns)
        if head == "studio":
            return _cmd_studio(ns)
        return _cmd_uic(ns)

    parser = build_parser()
    args = parser.parse_args(raw)
    return args.func(args)
