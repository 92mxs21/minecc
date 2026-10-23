"""Public-tree safety checks preventing accidental commits of proprietary game files."""

from __future__ import annotations

from pathlib import Path
import subprocess

FORBIDDEN_SUFFIXES: tuple[str, ...] = (
    ".jar",
    ".class",
    ".mrpack",
    ".mca",
    ".nbt",
)

FORBIDDEN_FILENAMES: frozenset[str] = frozenset(
    {
        "eula.txt",
        "client_mappings.txt",
        "server_mappings.txt",
        "MOJANGCS.SF",
        "MOJANGCS.RSA",
    }
)

FORBIDDEN_PATH_PREFIXES: tuple[str, ...] = (
    ".local/",
    "work/",
    "sources/",
    "decompiled/",
    "net/minecraft/",
    "com/mojang/",
    "assets/minecraft/",
    "assets/realms/",
    "data/minecraft/",
    "META-INF/versions/",
)


def classify_path_violation(rel_posix: str) -> str | None:
    """Return a human-readable violation reason if `rel_posix` must never be committed to Git."""
    norm = rel_posix.lstrip("./")
    name = Path(norm).name

    if name in FORBIDDEN_FILENAMES:
        return f"Forbidden file name ({name}) — local EULA/mappings/signatures must stay untracked."

    for prefix in FORBIDDEN_PATH_PREFIXES:
        if norm.startswith(prefix) or f"/{prefix}" in norm:
            return f"Forbidden path prefix ({prefix}) — Mojang code, assets, and local caches must not be committed."

    lower = norm.lower()
    for suffix in FORBIDDEN_SUFFIXES:
        if lower.endswith(suffix):
            return f"Forbidden binary/archive artifact (*{suffix}) — never commit game or bytecode archives."

    return None


def scan_paths(paths: list[str]) -> list[tuple[str, str]]:
    """Return `(path, reason)` tuples for every disallowed path in `paths`."""
    violations: list[tuple[str, str]] = []
    for path in sorted(set(paths)):
        reason = classify_path_violation(path)
        if reason is not None:
            violations.append((path, reason))
    return violations


def check_git_repository(repo_root: Path) -> list[tuple[str, str]]:
    """Inspect all tracked, staged, and non-ignored untracked files in `repo_root`."""
    repo_root = repo_root.resolve()
    proc = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    files = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    return scan_paths(files)
