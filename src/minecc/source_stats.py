"""Exact source-code metrics scanner for locally decompiled `.java` trees."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

FAILED_DECOMPILE_MARKERS: tuple[str, ...] = (
    "// $VF: Couldn't be decompiled",
    "// $FF: Couldn't be decompiled",
    "Unable to fully structure code",
    "// Byte code:",
)


@dataclass(frozen=True)
class FileLineCounts:
    """Line-level breakdown for a single `.java` file."""

    rel_path: str
    byte_size: int
    total_lines: int
    code_lines: int
    comment_lines: int
    blank_lines: int
    has_failure_marker: bool


@dataclass(frozen=True)
class SourceTreeStats:
    """Aggregated line, byte, package, and quality metrics across a decompiled directory."""

    root_dir: str
    java_files: int
    total_bytes: int
    total_lines: int
    code_lines: int
    comment_lines: int
    blank_lines: int
    files_with_failure_markers: int
    top_subsystems: list[dict[str, int | str]]
    largest_files: list[dict[str, int | str]]

    def to_dict(self) -> dict:
        return {
            "root_dir": self.root_dir,
            "java_files": self.java_files,
            "total_bytes": self.total_bytes,
            "total_lines": self.total_lines,
            "code_lines_sloc": self.code_lines,
            "comment_lines": self.comment_lines,
            "blank_lines": self.blank_lines,
            "files_with_failure_markers": self.files_with_failure_markers,
            "top_subsystems": self.top_subsystems,
            "largest_files": self.largest_files,
        }


def count_java_source(text: str, rel_path: str = "", byte_size: int | None = None) -> FileLineCounts:
    """Count total, SLOC, comment, and blank lines with a lexical Java comment/string scanner."""
    lines = text.splitlines()
    total_lines = len(lines)
    blank_lines = 0
    comment_lines = 0
    code_lines = 0

    in_block_comment = False

    for raw_line in lines:
        stripped = raw_line.strip()
        if not stripped:
            blank_lines += 1
            continue

        has_code_token = False
        has_comment_token = False
        in_string = False
        in_char = False
        i = 0
        n = len(stripped)

        while i < n:
            ch = stripped[i]
            nxt = stripped[i + 1] if i + 1 < n else ""

            if in_block_comment:
                has_comment_token = True
                if ch == "*" and nxt == "/":
                    in_block_comment = False
                    i += 2
                else:
                    i += 1
                continue

            if in_string:
                has_code_token = True
                if ch == "\\":
                    i += 2
                elif ch == '"':
                    in_string = False
                    i += 1
                else:
                    i += 1
                continue

            if in_char:
                has_code_token = True
                if ch == "\\":
                    i += 2
                elif ch == "'":
                    in_char = False
                    i += 1
                else:
                    i += 1
                continue

            if ch == "/" and nxt == "/":
                has_comment_token = True
                break
            if ch == "/" and nxt == "*":
                has_comment_token = True
                in_block_comment = True
                i += 2
                continue
            if ch == '"':
                has_code_token = True
                in_string = True
                i += 1
                continue
            if ch == "'":
                has_code_token = True
                in_char = True
                i += 1
                continue
            if not ch.isspace():
                has_code_token = True
            i += 1

        if has_code_token:
            code_lines += 1
        elif has_comment_token:
            comment_lines += 1
        else:
            blank_lines += 1

    has_marker = any(marker in text for marker in FAILED_DECOMPILE_MARKERS)
    return FileLineCounts(
        rel_path=rel_path,
        byte_size=byte_size if byte_size is not None else len(text.encode("utf-8")),
        total_lines=total_lines,
        code_lines=code_lines,
        comment_lines=comment_lines,
        blank_lines=blank_lines,
        has_failure_marker=has_marker,
    )


def _subsystem_key(rel_posix: str) -> str:
    """Bucket paths into readable top-level Minecraft subsystems (e.g. `net.minecraft.client`)."""
    parts = rel_posix.split("/")
    if len(parts) >= 4 and parts[0] == "net" and parts[1] == "minecraft":
        return ".".join(parts[:3])
    if len(parts) >= 4 and parts[0] == "com" and parts[1] == "mojang":
        return ".".join(parts[:3])
    if len(parts) > 1:
        return ".".join(parts[:-1])
    return "<root>"


def analyze_java_tree(root: Path) -> SourceTreeStats:
    """Scan a directory tree of `.java` files and compute exact SLOC and subsystem breakdowns."""
    root = root.expanduser().resolve()
    file_results: list[FileLineCounts] = []

    sub_files: dict[str, int] = defaultdict(int)
    sub_code: dict[str, int] = defaultdict(int)
    sub_total: dict[str, int] = defaultdict(int)

    for java_path in sorted(root.rglob("*.java")):
        if not java_path.is_file():
            continue
        raw = java_path.read_bytes()
        rel = java_path.relative_to(root).as_posix()
        counts = count_java_source(
            raw.decode("utf-8", errors="replace"),
            rel_path=rel,
            byte_size=len(raw),
        )
        file_results.append(counts)
        sub = _subsystem_key(rel)
        sub_files[sub] += 1
        sub_code[sub] += counts.code_lines
        sub_total[sub] += counts.total_lines

    top_subsystems = [
        {
            "subsystem": sub,
            "java_files": sub_files[sub],
            "code_lines_sloc": code,
            "total_lines": sub_total[sub],
        }
        for sub, code in sorted(sub_code.items(), key=lambda kv: kv[1], reverse=True)[:15]
    ]

    largest = [
        {
            "file": f.rel_path,
            "code_lines_sloc": f.code_lines,
            "total_lines": f.total_lines,
            "bytes": f.byte_size,
        }
        for f in sorted(file_results, key=lambda x: x.code_lines, reverse=True)[:20]
    ]

    return SourceTreeStats(
        root_dir=str(root),
        java_files=len(file_results),
        total_bytes=sum(f.byte_size for f in file_results),
        total_lines=sum(f.total_lines for f in file_results),
        code_lines=sum(f.code_lines for f in file_results),
        comment_lines=sum(f.comment_lines for f in file_results),
        blank_lines=sum(f.blank_lines for f in file_results),
        files_with_failure_markers=sum(1 for f in file_results if f.has_failure_marker),
        top_subsystems=top_subsystems,
        largest_files=largest,
    )
