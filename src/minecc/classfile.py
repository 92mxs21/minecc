"""JVMS §4 binary `.class` parser (Java SE 1.1 through Java SE 25+, classfile major 45..69+).

Pure Python standard-library implementation — reads compiled JVM class files directly
without executing bytecode or requiring a local JDK installation just to inspect a JAR.
"""

from __future__ import annotations

from dataclasses import dataclass
import struct

CLASS_MAGIC = 0xCAFEBABE

# Access flags (JVMS Table 4.1-B / 4.5-A / 4.6-A)
ACC_INTERFACE = 0x0200
ACC_ABSTRACT = 0x0400
ACC_SYNTHETIC = 0x1000
ACC_ANNOTATION = 0x2000
ACC_ENUM = 0x4000
ACC_MODULE = 0x8000

# Fixed byte sizes for constant-pool entries with non-variable payloads
_FIXED_CP_SIZES: dict[int, int] = {
    3: 4,  # CONSTANT_Integer
    4: 4,  # CONSTANT_Float
    5: 8,  # CONSTANT_Long (occupies 2 CP slots)
    6: 8,  # CONSTANT_Double (occupies 2 CP slots)
    7: 2,  # CONSTANT_Class
    8: 2,  # CONSTANT_String
    9: 4,  # CONSTANT_Fieldref
    10: 4,  # CONSTANT_Methodref
    11: 4,  # CONSTANT_InterfaceMethodref
    12: 4,  # CONSTANT_NameAndType
    15: 3,  # CONSTANT_MethodHandle
    16: 2,  # CONSTANT_MethodType
    17: 4,  # CONSTANT_Dynamic
    18: 4,  # CONSTANT_InvokeDynamic
    19: 2,  # CONSTANT_Module
    20: 2,  # CONSTANT_Package
}


class ClassFormatError(ValueError):
    """Raised when a byte stream is not a valid JVMS ClassFile structure."""


@dataclass(frozen=True, slots=True)
class ClassSummary:
    """Extracted structural and debug-symbol metadata for a single `.class` entry."""

    internal_name: str
    super_name: str | None
    package_name: str
    simple_name: str
    major_version: int
    minor_version: int
    access_flags: int
    field_count: int
    method_count: int
    interface_count: int
    bytecode_bytes: int
    source_file: str | None
    is_record: bool
    has_inner_dollar: bool
    min_line: int | None
    max_line: int | None
    executable_line_count: int
    member_names: tuple[str, ...]
    local_var_names: tuple[str, ...]

    @property
    def java_se_major(self) -> int:
        """Return target Java SE version (e.g., classfile 69 -> Java 25, 65 -> Java 21)."""
        return max(1, self.major_version - 44)

    @property
    def kind(self) -> str:
        """Classify class declaration flavor."""
        if self.access_flags & ACC_MODULE:
            return "module"
        if self.access_flags & ACC_ANNOTATION:
            return "annotation"
        if self.access_flags & ACC_INTERFACE:
            return "interface"
        if self.access_flags & ACC_ENUM:
            return "enum"
        if self.is_record:
            return "record"
        return "class"

    @property
    def canonical_source_path(self) -> str:
        """Expected `.java` output path (grouping nested/inner classes into enclosing file)."""
        if self.source_file:
            base = self.source_file
        else:
            top = self.simple_name.split("$", 1)[0]
            base = f"{top}.java"
        if self.package_name:
            return f"{self.package_name.replace('.', '/')}/{base}"
        return base


class _Reader:
    """Fast bounds-checked big-endian cursor over classfile bytes."""

    __slots__ = ("_buf", "pos")

    def __init__(self, buf: bytes | memoryview) -> None:
        self._buf = memoryview(buf)
        self.pos = 0

    def u1(self) -> int:
        end = self.pos + 1
        if end > len(self._buf):
            raise ClassFormatError("Unexpected EOF reading u1")
        val = self._buf[self.pos]
        self.pos = end
        return val

    def u2(self) -> int:
        end = self.pos + 2
        if end > len(self._buf):
            raise ClassFormatError("Unexpected EOF reading u2")
        (val,) = struct.unpack_from(">H", self._buf, self.pos)
        self.pos = end
        return val

    def u4(self) -> int:
        end = self.pos + 4
        if end > len(self._buf):
            raise ClassFormatError("Unexpected EOF reading u4")
        (val,) = struct.unpack_from(">I", self._buf, self.pos)
        self.pos = end
        return val

    def slice(self, length: int) -> memoryview:
        end = self.pos + length
        if end > len(self._buf):
            raise ClassFormatError(f"Unexpected EOF reading {length} bytes")
        out = self._buf[self.pos : end]
        self.pos = end
        return out

    def skip(self, length: int) -> None:
        end = self.pos + length
        if end > len(self._buf):
            raise ClassFormatError(f"Unexpected EOF skipping {length} bytes")
        self.pos = end


def _decode_modified_utf8(raw: memoryview) -> str:
    """Decode JVM Modified UTF-8 string bytes safely."""
    data = bytes(raw)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("latin-1", errors="replace")


def parse_class_bytes(data: bytes | memoryview) -> ClassSummary:
    """Parse a JVM `.class` binary buffer into a :class:`ClassSummary`."""
    r = _Reader(data)
    magic = r.u4()
    if magic != CLASS_MAGIC:
        raise ClassFormatError(f"Invalid JVM magic header: 0x{magic:08X}")

    minor_version = r.u2()
    major_version = r.u2()
    cp_count = r.u2()

    # Constant pool is 1-indexed [1 .. cp_count - 1]
    utf8_pool: dict[int, str] = {}
    class_name_idx: dict[int, int] = {}

    idx = 1
    while idx < cp_count:
        tag = r.u1()
        if tag == 1:  # CONSTANT_Utf8
            length = r.u2()
            utf8_pool[idx] = _decode_modified_utf8(r.slice(length))
            idx += 1
        elif tag == 7:  # CONSTANT_Class
            class_name_idx[idx] = r.u2()
            idx += 1
        elif tag in (5, 6):  # Long / Double take two constant-pool indices
            r.skip(8)
            idx += 2
        elif tag in _FIXED_CP_SIZES:
            r.skip(_FIXED_CP_SIZES[tag])
            idx += 1
        else:
            raise ClassFormatError(f"Unknown constant pool tag {tag} at index {idx}")

    def resolve_class(cp_index: int) -> str | None:
        if cp_index == 0:
            return None
        name_i = class_name_idx.get(cp_index)
        return utf8_pool.get(name_i, "") if name_i is not None else None

    access_flags = r.u2()
    this_class = resolve_class(r.u2()) or ""
    super_class = resolve_class(r.u2())

    interfaces_count = r.u2()
    r.skip(interfaces_count * 2)

    member_names: list[str] = []
    local_var_names: list[str] = []
    line_numbers: set[int] = set()
    bytecode_bytes = 0

    # Fields
    fields_count = r.u2()
    for _ in range(fields_count):
        f_flags = r.u2()
        f_name = utf8_pool.get(r.u2(), "")
        r.skip(2)  # descriptor_index
        if not (f_flags & ACC_SYNTHETIC) and f_name:
            member_names.append(f_name)
        f_attrs = r.u2()
        for _ in range(f_attrs):
            r.skip(2)
            r.skip(r.u4())

    # Methods
    methods_count = r.u2()
    for _ in range(methods_count):
        m_flags = r.u2()
        m_name = utf8_pool.get(r.u2(), "")
        r.skip(2)  # descriptor_index
        if not (m_flags & ACC_SYNTHETIC) and m_name and not m_name.startswith("<"):
            member_names.append(m_name)
        m_attrs = r.u2()
        for _ in range(m_attrs):
            attr_name = utf8_pool.get(r.u2(), "")
            attr_len = r.u4()
            if attr_name == "Code":
                code_slice = _Reader(r.slice(attr_len))
                code_slice.skip(4)  # max_stack (u2) + max_locals (u2)
                code_len = code_slice.u4()
                bytecode_bytes += code_len
                code_slice.skip(code_len)
                exc_len = code_slice.u2()
                code_slice.skip(exc_len * 8)
                sub_attrs = code_slice.u2()
                for _ in range(sub_attrs):
                    sub_name = utf8_pool.get(code_slice.u2(), "")
                    sub_len = code_slice.u4()
                    if sub_name == "LineNumberTable":
                        lnt = _Reader(code_slice.slice(sub_len))
                        entries = lnt.u2()
                        for _ in range(entries):
                            lnt.skip(2)  # start_pc
                            line_numbers.add(lnt.u2())
                    elif sub_name == "LocalVariableTable":
                        lvt = _Reader(code_slice.slice(sub_len))
                        entries = lvt.u2()
                        for _ in range(entries):
                            lvt.skip(4)  # start_pc + length
                            var_name = utf8_pool.get(lvt.u2(), "")
                            lvt.skip(4)  # descriptor_index + slot_index
                            if var_name and var_name != "this":
                                local_var_names.append(var_name)
                    else:
                        code_slice.skip(sub_len)
            else:
                r.skip(attr_len)

    # Class attributes
    source_file: str | None = None
    has_record_attr = False
    has_inner_attr = False
    class_attrs = r.u2()
    for _ in range(class_attrs):
        attr_name = utf8_pool.get(r.u2(), "")
        attr_len = r.u4()
        if attr_name == "SourceFile" and attr_len == 2:
            source_file = utf8_pool.get(r.u2())
        elif attr_name == "Record":
            has_record_attr = True
            r.skip(attr_len)
        elif attr_name == "InnerClasses":
            has_inner_attr = True
            r.skip(attr_len)
        else:
            r.skip(attr_len)

    if "/" in this_class:
        pkg_slash, simple_name = this_class.rsplit("/", 1)
        package_name = pkg_slash.replace("/", ".")
    else:
        package_name = ""
        simple_name = this_class

    is_inner = "$" in simple_name or (has_inner_attr and "$" in this_class)
    is_record = has_record_attr or super_class == "java/lang/Record"

    return ClassSummary(
        internal_name=this_class,
        super_name=super_class,
        package_name=package_name,
        simple_name=simple_name,
        major_version=major_version,
        minor_version=minor_version,
        access_flags=access_flags,
        field_count=fields_count,
        method_count=methods_count,
        interface_count=interfaces_count,
        bytecode_bytes=bytecode_bytes,
        source_file=source_file,
        is_record=is_record,
        has_inner_dollar=is_inner,
        min_line=min(line_numbers) if line_numbers else None,
        max_line=max(line_numbers) if line_numbers else None,
        executable_line_count=len(line_numbers),
        member_names=tuple(member_names),
        local_var_names=tuple(local_var_names),
    )
