"""Local clean-room Vineflower 1.12.0 decompilation runner for user-installed Minecraft JARs."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile

from minecc.jar_inspector import inspect_jar, load_research_lock
from minecc.source_stats import analyze_java_tree

EULA_URL = "https://www.minecraft.net/en-us/eula"


class DecompileSetupError(RuntimeError):
    """Raised when local prerequisites (EULA, Java runtime, output path safety) are not met."""


def ensure_eula_accepted(local_root: Path, accept_flag: bool) -> Path:
    """Gate local decompilation behind explicit per-machine Minecraft EULA acceptance."""
    local_root.mkdir(parents=True, exist_ok=True)
    eula_file = local_root / "eula.txt"

    if accept_flag:
        eula_file.write_text(
            f"# Local per-machine acknowledgment of the Minecraft EULA ({EULA_URL})\n"
            "# Do NOT commit or redistribute decompiled sources or game files.\n"
            "eula=true\n",
            encoding="utf-8",
        )
        return eula_file

    if eula_file.is_file():
        for line in eula_file.read_text(encoding="utf-8").splitlines():
            if line.strip().lower() == "eula=true":
                return eula_file

    eula_file.write_text(
        f"# Change eula=false to eula=true (or pass --accept-eula) after reviewing {EULA_URL}\n"
        "eula=false\n",
        encoding="utf-8",
    )
    raise DecompileSetupError(
        f"Minecraft EULA not yet accepted on this machine. Review {EULA_URL} and pass "
        f"--accept-eula (or set eula=true in {eula_file})."
    )


def ensure_safe_output_dir(out_dir: Path, repo_root: Path) -> Path:
    """Prevent writing proprietary decompiled sources into tracked paths inside the Git repo."""
    resolved_out = out_dir.expanduser().resolve()
    resolved_repo = repo_root.resolve()
    allowed_local = resolved_repo / ".local"

    if resolved_out == resolved_repo or resolved_repo in resolved_out.parents:
        if resolved_out != allowed_local and allowed_local not in resolved_out.parents:
            raise DecompileSetupError(
                f"Refusing to write decompiled Minecraft code into tracked repository path "
                f"{resolved_out}. Use a subdirectory inside {allowed_local} (git-ignored) "
                f"or an external directory outside {resolved_repo}."
            )
    return resolved_out


def find_java_executable(min_major: int = 17) -> tuple[str, int]:
    """Locate a local `java` runtime >= `min_major` (Java 25 recommended for MC 26.1+)."""
    candidates: list[Path] = []
    java_home = os.environ.get("JAVA_HOME")
    if java_home:
        candidates.append(Path(java_home) / "bin" / "java")
    which_java = shutil.which("java")
    if which_java:
        candidates.append(Path(which_java))

    for candidate in candidates:
        if not candidate.exists():
            continue
        proc = subprocess.run(
            [str(candidate), "-version"],
            capture_output=True,
            text=True,
            check=False,
        )
        banner = (proc.stderr or "") + "\n" + (proc.stdout or "")
        for token in banner.replace('"', " ").split():
            parts = token.split(".")
            if parts[0].isdigit():
                major = int(parts[1]) if parts[0] == "1" and len(parts) > 1 and parts[1].isdigit() else int(parts[0])
                if major >= min_major:
                    return str(candidate), major

    raise DecompileSetupError(
        f"No Java runtime >= {min_major} found on PATH / JAVA_HOME. "
        f"Install OpenJDK 25 (e.g. Temurin or Microsoft OpenJDK 25) to decompile Minecraft 26.x locally."
    )


def ensure_vineflower_jar(tools_dir: Path, lock_data: dict) -> Path:
    """Download and verify pinned Vineflower 1.12.0 JAR (Apache-2.0) into `.local/tools/`."""
    vf = lock_data["vineflower"]
    version = vf["version"]
    expected_sha256 = vf["sha256"]
    url = vf["url"]

    tools_dir.mkdir(parents=True, exist_ok=True)
    dest = tools_dir / f"vineflower-{version}.jar"

    if dest.is_file():
        actual = hashlib.sha256(dest.read_bytes()).hexdigest()
        if actual == expected_sha256:
            return dest
        dest.unlink()

    with urllib.request.urlopen(url, timeout=120) as resp:  # noqa: S310 - pinned HTTPS GitHub release URL
        payload = resp.read()

    actual = hashlib.sha256(payload).hexdigest()
    if actual != expected_sha256:
        raise DecompileSetupError(
            f"Vineflower SHA-256 mismatch: expected {expected_sha256}, got {actual}"
        )

    dest.write_bytes(payload)
    return dest


def collect_launcher_libraries(version_json: Path | None, minecraft_dir: Path | None) -> list[Path]:
    """Resolve local `<minecraft_dir>/libraries/...` JARs referenced by `<version>.json`."""
    if version_json is None or not version_json.is_file():
        return []
    mc_root = minecraft_dir.expanduser() if minecraft_dir else version_json.parents[2]
    libs_root = mc_root / "libraries"
    if not libs_root.is_dir():
        return []

    meta = json.loads(version_json.read_text(encoding="utf-8"))
    found: list[Path] = []
    seen: set[Path] = set()
    for lib in meta.get("libraries", []):
        rel = lib.get("downloads", {}).get("artifact", {}).get("path")
        if rel:
            candidate = (libs_root / rel).resolve()
            if candidate.is_file() and candidate not in seen:
                seen.add(candidate)
                found.append(candidate)
    return found


def run_local_decompile(
    jar_path: Path,
    repo_root: Path,
    version_hint: str | None = None,
    version_json: Path | None = None,
    minecraft_dir: Path | None = None,
    out_dir: Path | None = None,
    accept_eula: bool = False,
    threads: int | None = None,
    heap: str = "4G",
) -> dict:
    """Decompile a local Minecraft JAR with Vineflower 1.12.0 and compute exact code metrics."""
    lock_data = load_research_lock(repo_root)
    local_root = repo_root / ".local"
    ensure_eula_accepted(local_root, accept_flag=accept_eula)

    jar_report = inspect_jar(jar_path, version_hint=version_hint, lock_data=lock_data)
    if jar_report.obfuscation_status == "OBFUSCATED_PROGUARD":
        raise DecompileSetupError(
            f"{jar_path.name} is a pre-26.1 ProGuard-obfuscated build ({jar_report.obfuscation_reason}). "
            f"Remap it to named classes first with TinyRemapper + official Mojang mappings, or pass "
            f"an unobfuscated Minecraft 26.1+ JAR (such as 26.3)."
        )

    ver_label = jar_report.detected_version or "custom"
    target_out = ensure_safe_output_dir(
        out_dir if out_dir is not None else (local_root / "minecraft" / ver_label / "sources"),
        repo_root=repo_root,
    )
    reports_dir = local_root / "minecraft" / ver_label / "reports"
    reports_dir.mkdir(parents=True, exist_ok=True)
    target_out.mkdir(parents=True, exist_ok=True)

    # Extract inner server JAR if user passed Mojang's 1.18+ server bundler JAR
    input_jar_for_vf = jar_path.resolve()
    if jar_report.unwrapped_inner_jar:
        unwrapped_dest = local_root / "minecraft" / ver_label / "unwrapped-server.jar"
        unwrapped_dest.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(jar_path, "r") as zf:
            unwrapped_dest.write_bytes(zf.read(jar_report.unwrapped_inner_jar))
        input_jar_for_vf = unwrapped_dest

    java_bin, _ = find_java_executable(min_major=lock_data["vineflower"].get("min_runtime_java", 17))
    vf_jar = ensure_vineflower_jar(local_root / "tools", lock_data)
    lib_jars = collect_launcher_libraries(version_json, minecraft_dir)

    worker_count = threads or max(1, (os.cpu_count() or 4))
    cmd = [
        java_bin,
        f"-Xmx{heap}",
        "-jar",
        str(vf_jar),
        f"--threads={worker_count}",
        "--folder",
    ]
    for lib_jar in lib_jars:
        cmd.append(f"-e={lib_jar}")
    cmd.extend([str(input_jar_for_vf), str(target_out)])

    subprocess.run(cmd, check=True)

    src_stats = analyze_java_tree(target_out)
    combined = {
        "version": ver_label,
        "output_directory": str(target_out),
        "bytecode_inventory": jar_report.to_dict(),
        "decompiled_source_metrics": src_stats.to_dict(),
        "coverage": {
            "expected_java_files": jar_report.expected_java_files,
            "emitted_java_files": src_stats.java_files,
            "missing_java_files": max(0, jar_report.expected_java_files - src_stats.java_files),
            "files_with_failure_markers": src_stats.files_with_failure_markers,
        },
    }

    report_path = reports_dir / "audit-summary.json"
    report_path.write_text(json.dumps(combined, indent=2) + "\n", encoding="utf-8")
    combined["saved_report"] = str(report_path)
    return combined
