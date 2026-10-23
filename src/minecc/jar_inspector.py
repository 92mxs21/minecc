"""Local JAR discovery, SHA-1 lock verification, and JVMS bytecode inventory."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import zipfile

from minecc.classfile import ClassSummary, parse_class_bytes


def default_minecraft_dirs() -> list[Path]:
    """Return candidate local launcher directories across Linux, macOS, and Windows."""
    home = Path.home()
    candidates: list[Path] = []

    env_dir = os.environ.get("MINECRAFT_DIR")
    if env_dir:
        candidates.append(Path(env_dir).expanduser())

    system = platform.system()
    if system == "Windows":
        appdata = os.environ.get("APPDATA")
        if appdata:
            candidates.append(Path(appdata) / ".minecraft")
            candidates.append(Path(appdata) / "PrismLauncher" / "libraries" / "com" / "mojang" / "minecraft")
    elif system == "Darwin":
        candidates.append(home / "Library" / "Application Support" / "minecraft")
        candidates.append(home / "Library" / "Application Support" / "PrismLauncher")
    else:
        candidates.append(home / ".minecraft")
        candidates.append(home / ".local" / "share" / "PrismLauncher")
        candidates.append(home / ".var" / "app" / "org.prismlauncher.PrismLauncher" / "data" / "PrismLauncher")

    return candidates


def find_local_version_jar(version: str, minecraft_dir: Path | None = None) -> tuple[Path, Path | None]:
    """Locate `<version>.jar` (and optional `<version>.json`) from the user's own launcher install."""
    roots = [minecraft_dir.expanduser()] if minecraft_dir else default_minecraft_dirs()
    for root in roots:
        ver_dir = root / "versions" / version
        jar_path = ver_dir / f"{version}.jar"
        json_path = ver_dir / f"{version}.json"
        if jar_path.is_file():
            return jar_path, (json_path if json_path.is_file() else None)

        # PrismLauncher / MultiMC cached client library layout
        prism_jar = (
            root / "libraries" / "com" / "mojang" / "minecraft" / version / f"minecraft-{version}-client.jar"
        )
        if prism_jar.is_file():
            return prism_jar, None

    searched = ", ".join(str(p) for p in roots)
    raise FileNotFoundError(
        f"Could not find local Minecraft {version!r} client JAR in [{searched}]. "
        f"Install and launch {version} once in the official Minecraft Launcher, or pass --jar /path/to/{version}.jar."
    )


def load_research_lock(repo_root: Path | None = None) -> dict:
    """Load `research-lock.json` checksum pins and reference audit numbers."""
    if repo_root is None:
        repo_root = Path(__file__).resolve().parents[2]
    lock_file = repo_root / "research-lock.json"
    if not lock_file.is_file():
        return {"minecraft_versions": {}, "vineflower": {}}
    return json.loads(lock_file.read_text(encoding="utf-8"))


def compute_hashes(path: Path) -> tuple[str, str]:
    """Return `(sha1_hex, sha256_hex)` for a file."""
    h1 = hashlib.sha1()
    h256 = hashlib.sha256()
    with path.open("rb") as fh:
        while chunk := fh.read(1024 * 1024):
            h1.update(chunk)
            h256.update(chunk)
    return h1.hexdigest(), h256.hexdigest()


@dataclass(frozen=True)
class JarInspectionReport:
    """Structured bytecode and resource audit of a local Minecraft client/server JAR."""

    jar_path: str
    file_size_bytes: int
    sha1: str
    sha256: str
    unwrapped_inner_jar: str | None
    inner_jar_sha256: str | None
    detected_version: str | None
    lock_verification: str
    obfuscation_status: str
    obfuscation_reason: str
    java_se_versions: dict[str, int]
    input_classes: int
    top_level_classes: int
    inner_classes: int
    expected_java_files: int
    total_methods: int
    total_fields: int
    total_bytecode_bytes: int
    javac_source_line_span_total: int
    javac_executable_lines_total: int
    local_variable_symbols_count: int
    class_kinds: dict[str, int]
    namespace_class_counts: dict[str, int]
    top_packages_by_classes: list[dict[str, int | str]]
    resource_summary: dict[str, int]
    embedded_version_metadata: dict

    def to_dict(self) -> dict:
        return {
            "jar_path": self.jar_path,
            "file_size_bytes": self.file_size_bytes,
            "sha1": self.sha1,
            "sha256": self.sha256,
            "unwrapped_inner_jar": self.unwrapped_inner_jar,
            "inner_jar_sha256": self.inner_jar_sha256,
            "detected_version": self.detected_version,
            "lock_verification": self.lock_verification,
            "obfuscation_status": self.obfuscation_status,
            "obfuscation_reason": self.obfuscation_reason,
            "java_se_versions": self.java_se_versions,
            "bytecode_metrics": {
                "input_classes": self.input_classes,
                "top_level_classes": self.top_level_classes,
                "inner_classes": self.inner_classes,
                "expected_java_files": self.expected_java_files,
                "total_methods": self.total_methods,
                "total_fields": self.total_fields,
                "total_bytecode_bytes": self.total_bytecode_bytes,
                "javac_source_line_span_total": self.javac_source_line_span_total,
                "javac_executable_lines_total": self.javac_executable_lines_total,
                "local_variable_symbols_count": self.local_variable_symbols_count,
                "class_kinds": self.class_kinds,
                "namespace_class_counts": self.namespace_class_counts,
            },
            "top_packages_by_classes": self.top_packages_by_classes,
            "resource_summary": self.resource_summary,
            "embedded_version_metadata": self.embedded_version_metadata,
        }


def _classify_obfuscation(classes: list[ClassSummary]) -> tuple[str, str]:
    """Detect whether bytecode ships unobfuscated (26.1+) or ProGuard-obfuscated (<=1.21.11)."""
    if not classes:
        return "UNKNOWN", "No .class entries found in archive."

    mc_pkg_classes = sum(
        1 for c in classes if c.package_name.startswith(("net.minecraft", "com.mojang"))
    )
    root_short_classes = sum(
        1 for c in classes if not c.package_name and 1 <= len(c.simple_name.split("$", 1)[0]) <= 4
    )
    all_members = [m for c in classes for m in c.member_names]
    short_members = sum(1 for m in all_members if len(m) <= 2)
    lvt_total = sum(len(c.local_var_names) for c in classes)

    short_member_ratio = (short_members / len(all_members)) if all_members else 0.0
    mc_pkg_ratio = mc_pkg_classes / len(classes)

    if root_short_classes > 25 or (mc_pkg_ratio < 0.5 and short_member_ratio > 0.35):
        return (
            "OBFUSCATED_PROGUARD",
            f"Detected {root_short_classes} root short-named classes and "
            f"{short_member_ratio:.1%} 1-2 char member names (pre-26.1 obfuscated JAR; remapping required).",
        )

    return (
        "UNOBFUSCATED",
        f"{mc_pkg_classes}/{len(classes)} ({mc_pkg_ratio:.1%}) classes already carry readable packages "
        f"({short_member_ratio:.1%} short member names, {lvt_total:,} LocalVariableTable symbols) — "
        f"ready for direct Vineflower decompilation with no mapping step.",
    )


def inspect_jar(
    jar_path: Path,
    version_hint: str | None = None,
    lock_data: dict | None = None,
) -> JarInspectionReport:
    """Inspect every `.class` and resource entry inside a client or server JAR archive."""
    jar_path = jar_path.expanduser().resolve()
    sha1, sha256 = compute_hashes(jar_path)
    size_bytes = jar_path.stat().st_size
    locks = (lock_data if lock_data is not None else load_research_lock()).get("minecraft_versions", {})

    unwrapped_entry: str | None = None
    inner_sha256: str | None = None

    with zipfile.ZipFile(jar_path, "r") as outer_zf:
        names = outer_zf.namelist()
        # Unwrap Mojang's 1.18+ server bundler (`META-INF/versions/<ver>/server-<ver>.jar`) automatically
        inner_candidates = [
            n for n in names if n.startswith("META-INF/versions/") and n.endswith(".jar")
        ]
        if inner_candidates and not any(n.startswith("net/minecraft/") and n.endswith(".class") for n in names):
            unwrapped_entry = inner_candidates[0]
            raw_inner = outer_zf.read(unwrapped_entry)
            inner_sha256 = hashlib.sha256(raw_inner).hexdigest()
            target_zf = zipfile.ZipFile(io.BytesIO(raw_inner), "r")
        else:
            target_zf = outer_zf

        try:
            classes: list[ClassSummary] = []
            resource_counts: Counter[str] = Counter()
            version_meta: dict = {}

            for info in target_zf.infolist():
                if info.is_dir():
                    continue
                entry = info.filename
                if entry.endswith(".class"):
                    summary = parse_class_bytes(target_zf.read(info))
                    classes.append(summary)
                else:
                    if entry == "version.json":
                        try:
                            version_meta = json.loads(target_zf.read(info).decode("utf-8"))
                        except (UnicodeDecodeError, json.JSONDecodeError):
                            version_meta = {}
                    if entry.startswith("assets/"):
                        resource_counts["assets_files"] += 1
                        if entry.endswith((".fsh", ".vsh", ".glsl")):
                            resource_counts["shader_files"] += 1
                        elif entry.endswith(".png"):
                            resource_counts["texture_png_files"] += 1
                    elif entry.startswith("data/"):
                        resource_counts["datapack_files"] += 1
                    elif entry.startswith("META-INF/"):
                        resource_counts["meta_inf_files"] += 1
                    else:
                        resource_counts["other_root_files"] += 1
        finally:
            if target_zf is not outer_zf:
                target_zf.close()

    detected_version = version_meta.get("id") or version_meta.get("name") or version_hint

    # Match SHA-1 / SHA-256 against research-lock.json
    lock_status = "unpinned"
    for ver_id, pin in locks.items():
        if sha1 in (pin.get("client_sha1"), pin.get("server_sha1")):
            detected_version = detected_version or ver_id
            artifact_role = "client.jar" if sha1 == pin.get("client_sha1") else "server.jar"
            lock_status = f"verified ({ver_id} {artifact_role} matches pinned SHA-1)"
            break
    else:
        if detected_version and detected_version in locks:
            pin = locks[detected_version]
            expected_hashes = {h for h in (pin.get("client_sha1"), pin.get("server_sha1")) if h}
            if expected_hashes:
                lock_status = f"mismatch (SHA-1 {sha1} does not match pinned {detected_version} hash)"

    obf_status, obf_reason = _classify_obfuscation(classes)

    # Group by canonical `.java` source file path (nesting `$` inner classes cleanly)
    source_max_line: dict[str, int] = defaultdict(int)
    pkg_class_counter: Counter[str] = Counter()
    pkg_file_sets: dict[str, set[str]] = defaultdict(set)
    namespace_counter: Counter[str] = Counter()
    kind_counter: Counter[str] = Counter()
    java_ver_counter: Counter[str] = Counter()

    inner_count = 0
    total_methods = 0
    total_fields = 0
    total_bytecode_bytes = 0
    total_exec_lines = 0
    total_lvt = 0

    for cls in classes:
        src_path = cls.canonical_source_path
        if cls.max_line is not None and cls.max_line > source_max_line[src_path]:
            source_max_line[src_path] = cls.max_line
        else:
            source_max_line.setdefault(src_path, 0)

        pkg = cls.package_name or "<root>"
        pkg_class_counter[pkg] += 1
        pkg_file_sets[pkg].add(src_path)

        if cls.package_name.startswith("net.minecraft"):
            namespace_counter["net.minecraft.*"] += 1
        elif cls.package_name.startswith("com.mojang"):
            namespace_counter["com.mojang.*"] += 1
        else:
            namespace_counter["other"] += 1

        kind_counter[cls.kind] += 1
        java_ver_counter[f"Java SE {cls.java_se_major} (classfile {cls.major_version}.{cls.minor_version})"] += 1

        if cls.has_inner_dollar:
            inner_count += 1
        total_methods += cls.method_count
        total_fields += cls.field_count
        total_bytecode_bytes += cls.bytecode_bytes
        total_exec_lines += cls.executable_line_count
        total_lvt += len(cls.local_var_names)

    top_packages = [
        {
            "package": pkg,
            "classes": count,
            "java_files": len(pkg_file_sets[pkg]),
        }
        for pkg, count in pkg_class_counter.most_common(15)
    ]

    return JarInspectionReport(
        jar_path=str(jar_path),
        file_size_bytes=size_bytes,
        sha1=sha1,
        sha256=sha256,
        unwrapped_inner_jar=unwrapped_entry,
        inner_jar_sha256=inner_sha256,
        detected_version=detected_version,
        lock_verification=lock_status,
        obfuscation_status=obf_status,
        obfuscation_reason=obf_reason,
        java_se_versions=dict(java_ver_counter),
        input_classes=len(classes),
        top_level_classes=len(classes) - inner_count,
        inner_classes=inner_count,
        expected_java_files=len(source_max_line),
        total_methods=total_methods,
        total_fields=total_fields,
        total_bytecode_bytes=total_bytecode_bytes,
        javac_source_line_span_total=sum(source_max_line.values()),
        javac_executable_lines_total=total_exec_lines,
        local_variable_symbols_count=total_lvt,
        class_kinds=dict(kind_counter),
        namespace_class_counts=dict(namespace_counter),
        top_packages_by_classes=top_packages,
        resource_summary=dict(resource_counts),
        embedded_version_metadata=version_meta,
    )
