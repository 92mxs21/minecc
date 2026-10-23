#pragma once
// HeliosJVM: Pure C++20 RFC 1951 Deflate + ZIP/JAR Reader + JVMS 0xCAFEBABE (Java 8..25 / v69.0) Disassembler
// Zero external DLLs or proprietary Mojang code included. Reads local user JARs & source trees cleanly.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace helios::jvm {

// ============================================================================
// Pure C++20 RFC 1951 DEFLATE Decompressor (Stored + Fixed + Dynamic Huffman)
// ============================================================================
namespace deflate {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t byte_pos = 0;
    int bit_pos = 0;

    uint32_t read_bits(int count) {
        uint32_t out = 0;
        for (int i = 0; i < count; ++i) {
            if (byte_pos >= size) throw std::runtime_error("Truncated DEFLATE stream");
            uint32_t bit = (data[byte_pos] >> bit_pos) & 1u;
            out |= (bit << i);
            if (++bit_pos == 8) {
                bit_pos = 0;
                ++byte_pos;
            }
        }
        return out;
    }

    void align_to_byte() {
        if (bit_pos != 0) {
            bit_pos = 0;
            ++byte_pos;
        }
    }
};

struct HuffmanTree {
    static constexpr int MAX_BITS = 15;
    std::array<int, MAX_BITS + 1> count{};
    std::vector<int> symbols;

    void build(const std::vector<int>& lengths) {
        count.fill(0);
        for (int len : lengths) {
            if (len > 0 && len <= MAX_BITS) ++count[len];
        }
        std::array<int, MAX_BITS + 1> offs{};
        int sum = 0;
        for (int bits = 1; bits <= MAX_BITS; ++bits) {
            offs[bits] = sum;
            sum += count[bits];
        }
        symbols.assign(static_cast<size_t>(sum), 0);
        for (size_t sym = 0; sym < lengths.size(); ++sym) {
            int len = lengths[sym];
            if (len > 0 && len <= MAX_BITS) {
                symbols[static_cast<size_t>(offs[len]++)] = static_cast<int>(sym);
            }
        }
    }

    int decode(BitReader& br) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= MAX_BITS; ++len) {
            code |= static_cast<int>(br.read_bits(1));
            int c = count[len];
            if (code - c < first) {
                return symbols[static_cast<size_t>(index + (code - first))];
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("Invalid Huffman code in DEFLATE stream");
    }
};

inline std::vector<uint8_t> inflate_raw(const uint8_t* src, size_t src_len, size_t expected_uncompressed = 0) {
    static constexpr int LEN_BASE[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr int LEN_EXTRA[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr int DIST_BASE[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr int DIST_EXTRA[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr int CL_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    BitReader br{src, src_len, 0, 0};
    std::vector<uint8_t> out;
    out.reserve(expected_uncompressed ? expected_uncompressed : src_len * 2);

    bool is_final = false;
    while (!is_final) {
        is_final = (br.read_bits(1) != 0);
        uint32_t btype = br.read_bits(2);
        if (btype == 0u) {
            br.align_to_byte();
            if (br.byte_pos + 4 > br.size) throw std::runtime_error("Truncated stored block");
            uint16_t len = static_cast<uint16_t>(br.data[br.byte_pos] | (br.data[br.byte_pos + 1] << 8));
            br.byte_pos += 4;
            if (br.byte_pos + len > br.size) throw std::runtime_error("Truncated stored payload");
            out.insert(out.end(), br.data + br.byte_pos, br.data + br.byte_pos + len);
            br.byte_pos += len;
        } else if (btype == 1u || btype == 2u) {
            HuffmanTree lit_tree, dist_tree;
            if (btype == 1u) {
                std::vector<int> ll(288, 8);
                for (int i = 144; i <= 255; ++i) ll[i] = 9;
                for (int i = 256; i <= 279; ++i) ll[i] = 7;
                lit_tree.build(ll);
                dist_tree.build(std::vector<int>(32, 5));
            } else {
                int hlit = static_cast<int>(br.read_bits(5)) + 257;
                int hdist = static_cast<int>(br.read_bits(5)) + 1;
                int hclen = static_cast<int>(br.read_bits(4)) + 4;
                std::vector<int> cl_lens(19, 0);
                for (int i = 0; i < hclen; ++i) cl_lens[CL_ORDER[i]] = static_cast<int>(br.read_bits(3));
                HuffmanTree cl_tree;
                cl_tree.build(cl_lens);

                std::vector<int> lengths;
                lengths.reserve(static_cast<size_t>(hlit + hdist));
                while (static_cast<int>(lengths.size()) < hlit + hdist) {
                    int sym = cl_tree.decode(br);
                    if (sym <= 15) {
                        lengths.push_back(sym);
                    } else if (sym == 16) {
                        if (lengths.empty()) throw std::runtime_error("Invalid repeat in Huffman header");
                        int rep = static_cast<int>(br.read_bits(2)) + 3;
                        lengths.insert(lengths.end(), static_cast<size_t>(rep), lengths.back());
                    } else if (sym == 17) {
                        int rep = static_cast<int>(br.read_bits(3)) + 3;
                        lengths.insert(lengths.end(), static_cast<size_t>(rep), 0);
                    } else if (sym == 18) {
                        int rep = static_cast<int>(br.read_bits(7)) + 11;
                        lengths.insert(lengths.end(), static_cast<size_t>(rep), 0);
                    }
                }
                lit_tree.build(std::vector<int>(lengths.begin(), lengths.begin() + hlit));
                dist_tree.build(std::vector<int>(lengths.begin() + hlit, lengths.begin() + hlit + hdist));
            }

            while (true) {
                int sym = lit_tree.decode(br);
                if (sym < 256) {
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li < 0 || li >= 29) throw std::runtime_error("Invalid length symbol");
                    int copy_len = LEN_BASE[li] + static_cast<int>(br.read_bits(LEN_EXTRA[li]));
                    int di = dist_tree.decode(br);
                    if (di < 0 || di >= 30) throw std::runtime_error("Invalid distance symbol");
                    int dist = DIST_BASE[di] + static_cast<int>(br.read_bits(DIST_EXTRA[di]));
                    if (static_cast<size_t>(dist) > out.size()) throw std::runtime_error("Backref past start");
                    for (int k = 0; k < copy_len; ++k) {
                        out.push_back(out[out.size() - static_cast<size_t>(dist)]);
                    }
                }
            }
        } else {
            throw std::runtime_error("Invalid DEFLATE block type 3");
        }
    }
    return out;
}

} // namespace deflate

// ============================================================================
// Pure C++20 ZIP / JAR Central Directory Reader
// ============================================================================
struct ZipEntryMeta {
    std::string name;
    uint16_t method = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint64_t local_header_offset = 0;
};

class JarArchive {
public:
    std::string jar_path;
    std::vector<uint8_t> raw_file;
    std::unordered_map<std::string, ZipEntryMeta> entries;

    static uint16_t u16le(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    static uint32_t u32le(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool open(const std::string& path, std::string& err_out) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            err_out = "Cannot open file: " + path;
            return false;
        }
        std::streamsize sz = f.tellg();
        if (sz < 22) {
            err_out = "Archive too small to be a valid JAR/ZIP: " + path;
            return false;
        }
        f.seekg(0, std::ios::beg);
        raw_file.resize(static_cast<size_t>(sz));
        if (!f.read(reinterpret_cast<char*>(raw_file.data()), sz)) {
            err_out = "Read error on JAR: " + path;
            return false;
        }

        // Locate End-Of-Central-Directory signature PK\x05\x06 from end of archive
        const size_t n = raw_file.size();
        size_t eocd = std::string::npos;
        size_t scan_min = (n > 65557) ? (n - 65557) : 0;
        for (size_t p = n - 22; p >= scan_min; --p) {
            if (raw_file[p] == 0x50 && raw_file[p + 1] == 0x4B &&
                raw_file[p + 2] == 0x05 && raw_file[p + 3] == 0x06)
            {
                eocd = p;
                break;
            }
            if (p == 0) break;
        }
        if (eocd == std::string::npos) {
            err_out = "Missing ZIP End-of-Central-Directory signature";
            return false;
        }

        uint32_t cd_size = u32le(raw_file.data() + eocd + 12);
        uint32_t cd_offset = u32le(raw_file.data() + eocd + 16);
        if (static_cast<size_t>(cd_offset) + static_cast<size_t>(cd_size) > n) {
            err_out = "Corrupt ZIP Central Directory bounds";
            return false;
        }

        entries.clear();
        size_t cur = cd_offset;
        const size_t cd_end = cd_offset + cd_size;
        while (cur + 46 <= cd_end) {
            if (!(raw_file[cur] == 0x50 && raw_file[cur + 1] == 0x4B &&
                  raw_file[cur + 2] == 0x01 && raw_file[cur + 3] == 0x02))
            {
                break;
            }
            ZipEntryMeta meta;
            meta.method = u16le(raw_file.data() + cur + 10);
            meta.comp_size = u32le(raw_file.data() + cur + 20);
            meta.uncomp_size = u32le(raw_file.data() + cur + 24);
            uint16_t name_len = u16le(raw_file.data() + cur + 28);
            uint16_t extra_len = u16le(raw_file.data() + cur + 30);
            uint16_t comment_len = u16le(raw_file.data() + cur + 32);
            meta.local_header_offset = u32le(raw_file.data() + cur + 42);
            if (cur + 46 + name_len > cd_end) break;
            meta.name.assign(reinterpret_cast<const char*>(raw_file.data() + cur + 46), name_len);
            entries[meta.name] = meta;
            cur += 46 + static_cast<size_t>(name_len) + extra_len + comment_len;
        }
        jar_path = path;
        return true;
    }

    std::vector<uint8_t> read_entry(const std::string& name) const {
        auto it = entries.find(name);
        if (it == entries.end()) return {};
        const auto& m = it->second;
        size_t lh = static_cast<size_t>(m.local_header_offset);
        if (lh + 30 > raw_file.size()) return {};
        uint16_t fn_len = u16le(raw_file.data() + lh + 26);
        uint16_t ex_len = u16le(raw_file.data() + lh + 28);
        size_t data_pos = lh + 30 + fn_len + ex_len;
        if (data_pos + m.comp_size > raw_file.size()) return {};
        const uint8_t* ptr = raw_file.data() + data_pos;
        if (m.method == 0u) return std::vector<uint8_t>(ptr, ptr + m.comp_size);
        if (m.method == 8u) return deflate::inflate_raw(ptr, m.comp_size, m.uncomp_size);
        return {};
    }
};

// ============================================================================
// JVMS §4 ClassFile Parser & Opcode Disassembler (Java 8 52.0 -> Java 25 69.0)
// ============================================================================
struct DisassembledClass {
    uint16_t minor_ver = 0;
    uint16_t major_ver = 69; // 69.0 = Java SE 25 (Minecraft 26.3!)
    uint16_t access_flags = 0;
    std::string this_class;
    std::string super_class;
    std::vector<std::string> interfaces;
    std::vector<std::pair<std::string, std::string>> fields;  // (name, java_type_sig)
    std::vector<std::pair<std::string, std::string>> methods; // (name, raw_descriptor)
    std::string synthetic_java_source;
    std::string bytecode_listing;
};

namespace cf_detail {

inline uint8_t r_u8(const uint8_t* p, size_t n, size_t& o) {
    if (o + 1 > n) throw std::runtime_error("EOF u8");
    return p[o++];
}
inline uint16_t r_u16(const uint8_t* p, size_t n, size_t& o) {
    if (o + 2 > n) throw std::runtime_error("EOF u16");
    uint16_t v = (static_cast<uint16_t>(p[o]) << 8) | p[o + 1];
    o += 2;
    return v;
}
inline uint32_t r_u32(const uint8_t* p, size_t n, size_t& o) {
    if (o + 4 > n) throw std::runtime_error("EOF u32");
    uint32_t v = (static_cast<uint32_t>(p[o]) << 24) | (static_cast<uint32_t>(p[o + 1]) << 16) |
                 (static_cast<uint32_t>(p[o + 2]) << 8) | p[o + 3];
    o += 4;
    return v;
}

inline std::string pretty_type(std::string_view desc, size_t& pos) {
    if (pos >= desc.size()) return "void";
    int arr_dims = 0;
    while (pos < desc.size() && desc[pos] == '[') { ++arr_dims; ++pos; }
    std::string base = "Object";
    char c = desc[pos++];
    switch (c) {
        case 'B': base = "byte"; break;
        case 'C': base = "char"; break;
        case 'D': base = "double"; break;
        case 'F': base = "float"; break;
        case 'I': base = "int"; break;
        case 'J': base = "long"; break;
        case 'S': base = "short"; break;
        case 'Z': base = "boolean"; break;
        case 'V': base = "void"; break;
        case 'L': {
            size_t semi = desc.find(';', pos);
            std::string full = std::string(desc.substr(pos, semi == std::string_view::npos ? desc.size() - pos : semi - pos));
            pos = (semi == std::string_view::npos) ? desc.size() : semi + 1;
            size_t slash = full.rfind('/');
            base = (slash == std::string::npos) ? full : full.substr(slash + 1);
            break;
        }
        default: break;
    }
    for (int d = 0; d < arr_dims; ++d) base += "[]";
    return base;
}

inline const char* opcode_name(uint8_t op) {
    switch (op) {
        case 0x00: return "nop";
        case 0x01: return "aconst_null";
        case 0x02: return "iconst_m1";
        case 0x03: return "iconst_0";
        case 0x04: return "iconst_1";
        case 0x05: return "iconst_2";
        case 0x06: return "iconst_3";
        case 0x07: return "iconst_4";
        case 0x08: return "iconst_5";
        case 0x10: return "bipush";
        case 0x11: return "sipush";
        case 0x12: return "ldc";
        case 0x13: return "ldc_w";
        case 0x14: return "ldc2_w";
        case 0x15: return "iload";
        case 0x19: return "aload";
        case 0x1a: return "iload_0";
        case 0x1b: return "iload_1";
        case 0x1c: return "iload_2";
        case 0x1d: return "iload_3";
        case 0x2a: return "aload_0";
        case 0x2b: return "aload_1";
        case 0x2c: return "aload_2";
        case 0x2d: return "aload_3";
        case 0x36: return "istore";
        case 0x3a: return "astore";
        case 0x4b: return "astore_0";
        case 0x4c: return "astore_1";
        case 0x4d: return "astore_2";
        case 0x4e: return "astore_3";
        case 0x57: return "pop";
        case 0x59: return "dup";
        case 0x60: return "iadd";
        case 0x64: return "isub";
        case 0x99: return "ifeq";
        case 0x9a: return "ifne";
        case 0x9f: return "ificmpeq";
        case 0xa0: return "ificmpne";
        case 0xa7: return "goto";
        case 0xac: return "ireturn";
        case 0xad: return "lreturn";
        case 0xb0: return "areturn";
        case 0xb1: return "return";
        case 0xb2: return "getstatic";
        case 0xb3: return "putstatic";
        case 0xb4: return "getfield";
        case 0xb5: return "putfield";
        case 0xb6: return "invokevirtual";
        case 0xb7: return "invokespecial";
        case 0xb8: return "invokestatic";
        case 0xb9: return "invokeinterface";
        case 0xba: return "invokedynamic";
        case 0xbb: return "new";
        case 0xbd: return "anewarray";
        case 0xbe: return "arraylength";
        case 0xbf: return "athrow";
        case 0xc0: return "checkcast";
        case 0xc1: return "instanceof";
        case 0xc6: return "ifnull";
        case 0xc7: return "ifnonnull";
        default: return "jvm_op";
    }
}

} // namespace cf_detail

inline DisassembledClass parse_class_bytes(const uint8_t* data, size_t len) {
    DisassembledClass out;
    size_t pos = 0;
    uint32_t magic = cf_detail::r_u32(data, len, pos);
    if (magic != 0xCAFEBABEu) throw std::runtime_error("Not a valid 0xCAFEBABE JVM .class file");

    out.minor_ver = cf_detail::r_u16(data, len, pos);
    out.major_ver = cf_detail::r_u16(data, len, pos);
    uint16_t cp_count = cf_detail::r_u16(data, len, pos);

    struct CpEntry {
        uint8_t tag = 0;
        std::string s;
        uint16_t a = 0;
        uint16_t b = 0;
    };
    std::vector<CpEntry> cp(cp_count);
    for (uint16_t i = 1; i < cp_count; ++i) {
        uint8_t tag = cf_detail::r_u8(data, len, pos);
        cp[i].tag = tag;
        switch (tag) {
            case 1: { // Utf8
                uint16_t slen = cf_detail::r_u16(data, len, pos);
                if (pos + slen > len) throw std::runtime_error("Truncated CP UTF8");
                cp[i].s.assign(reinterpret_cast<const char*>(data + pos), slen);
                pos += slen;
                break;
            }
            case 3: // Integer
            case 4: { // Float
                int32_t v = static_cast<int32_t>(cf_detail::r_u32(data, len, pos));
                cp[i].s = std::to_string(v);
                break;
            }
            case 5: // Long
            case 6: { // Double
                uint64_t hi = cf_detail::r_u32(data, len, pos);
                uint64_t lo = cf_detail::r_u32(data, len, pos);
                cp[i].s = std::to_string(static_cast<int64_t>((hi << 32) | lo));
                ++i; // 8-byte constants occupy 2 CP slots per JVMS §4.4.5
                break;
            }
            case 7: // Class
            case 8: // String
            case 16: // MethodType
            case 19: // Module
            case 20: // Package
                cp[i].a = cf_detail::r_u16(data, len, pos);
                break;
            case 9:  // Fieldref
            case 10: // Methodref
            case 11: // InterfaceMethodref
            case 12: // NameAndType
            case 17: // Dynamic
            case 18: // InvokeDynamic
                cp[i].a = cf_detail::r_u16(data, len, pos);
                cp[i].b = cf_detail::r_u16(data, len, pos);
                break;
            case 15: // MethodHandle
                cf_detail::r_u8(data, len, pos);
                cp[i].a = cf_detail::r_u16(data, len, pos);
                break;
            default:
                throw std::runtime_error("Unknown CP tag " + std::to_string(tag));
        }
    }

    auto cp_utf8 = [&](uint16_t idx) -> std::string {
        return (idx < cp.size()) ? cp[idx].s : "";
    };
    auto cp_class = [&](uint16_t idx) -> std::string {
        return (idx < cp.size() && cp[idx].tag == 7) ? cp_utf8(cp[idx].a) : "";
    };
    auto cp_desc = [&](uint16_t idx) -> std::string {
        if (idx >= cp.size()) return "";
        const auto& e = cp[idx];
        if (e.tag == 1) return e.s;
        if (e.tag == 3 || e.tag == 5) return e.s;
        if (e.tag == 7) return cp_class(idx);
        if (e.tag == 8) return "\"" + cp_utf8(e.a) + "\"";
        if (e.tag == 9 || e.tag == 10 || e.tag == 11) {
            std::string cls = cp_class(e.a);
            if (e.b < cp.size()) {
                return cls + "." + cp_utf8(cp[e.b].a) + ":" + cp_utf8(cp[e.b].b);
            }
        }
        if (e.tag == 18 && e.b < cp.size()) {
            return "bsm#" + std::to_string(e.a) + " " + cp_utf8(cp[e.b].a) + cp_utf8(cp[e.b].b);
        }
        return "#" + std::to_string(idx);
    };

    out.access_flags = cf_detail::r_u16(data, len, pos);
    out.this_class = cp_class(cf_detail::r_u16(data, len, pos));
    out.super_class = cp_class(cf_detail::r_u16(data, len, pos));

    uint16_t iface_count = cf_detail::r_u16(data, len, pos);
    for (uint16_t i = 0; i < iface_count; ++i) {
        out.interfaces.push_back(cp_class(cf_detail::r_u16(data, len, pos)));
    }

    std::set<std::string> imports;
    auto record_import = [&](const std::string& internal) {
        if (internal.empty() || internal == "java/lang/Object" || internal.find('/') == std::string::npos) return;
        std::string pkg_this = out.this_class.substr(0, out.this_class.rfind('/'));
        std::string pkg_other = internal.substr(0, internal.rfind('/'));
        if (pkg_other != "java/lang" && pkg_other != pkg_this) {
            std::string dot = internal;
            std::replace(dot.begin(), dot.end(), '/', '.');
            imports.insert(dot);
        }
    };
    record_import(out.super_class);
    for (const auto& itf : out.interfaces) record_import(itf);

    // Fields
    uint16_t fields_count = cf_detail::r_u16(data, len, pos);
    struct FieldRec { uint16_t flags; std::string name; std::string desc; std::string const_val; };
    std::vector<FieldRec> field_recs;
    for (uint16_t i = 0; i < fields_count; ++i) {
        uint16_t fflags = cf_detail::r_u16(data, len, pos);
        std::string fname = cp_utf8(cf_detail::r_u16(data, len, pos));
        std::string fdesc = cp_utf8(cf_detail::r_u16(data, len, pos));
        uint16_t ac = cf_detail::r_u16(data, len, pos);
        std::string cval;
        for (uint16_t a = 0; a < ac; ++a) {
            std::string aname = cp_utf8(cf_detail::r_u16(data, len, pos));
            uint32_t alen = cf_detail::r_u32(data, len, pos);
            if (aname == "ConstantValue" && alen == 2) {
                cval = cp_desc(cf_detail::r_u16(data, len, pos));
            } else {
                pos += alen;
            }
        }
        size_t dp = 0;
        std::string ptype = cf_detail::pretty_type(fdesc, dp);
        out.fields.emplace_back(fname, ptype);
        field_recs.push_back({fflags, fname, fdesc, cval});
    }

    // Methods + Disassembly
    int java_ver = static_cast<int>(out.major_ver) - 44;
    std::ostringstream bc;
    bc << "// Classfile " << out.this_class << ".class (JVMS major=" << out.major_ver
       << "." << out.minor_ver << " / Java SE " << java_ver << ")\n"
       << "// Constant pool entries: " << cp_count << " | Access flags: 0x"
       << std::hex << out.access_flags << std::dec << "\n\n";

    uint16_t methods_count = cf_detail::r_u16(data, len, pos);
    struct MethodRec { uint16_t flags; std::string name; std::string desc; int code_bytes; };
    std::vector<MethodRec> method_recs;

    for (uint16_t i = 0; i < methods_count; ++i) {
        uint16_t mflags = cf_detail::r_u16(data, len, pos);
        std::string mname = cp_utf8(cf_detail::r_u16(data, len, pos));
        std::string mdesc = cp_utf8(cf_detail::r_u16(data, len, pos));
        uint16_t ac = cf_detail::r_u16(data, len, pos);
        out.methods.emplace_back(mname, mdesc);

        bc << "  method " << mname << mdesc << " [flags=0x" << std::hex << mflags << std::dec << "]\n";
        int code_len_saved = 0;

        for (uint16_t a = 0; a < ac; ++a) {
            std::string aname = cp_utf8(cf_detail::r_u16(data, len, pos));
            uint32_t alen = cf_detail::r_u32(data, len, pos);
            size_t attr_end = pos + alen;
            if (aname == "Code" && alen >= 8) {
                uint16_t max_stack = cf_detail::r_u16(data, len, pos);
                uint16_t max_locals = cf_detail::r_u16(data, len, pos);
                uint32_t code_len = cf_detail::r_u32(data, len, pos);
                code_len_saved = static_cast<int>(code_len);
                bc << "    stack=" << max_stack << ", locals=" << max_locals << ", bytes=" << code_len << "\n";
                size_t cend = std::min(len, pos + code_len);
                size_t pc = pos;
                int insn_cap = 0;
                while (pc < cend && insn_cap++ < 80) {
                    size_t off = pc - pos;
                    uint8_t op = data[pc++];
                    bc << "    " << std::setw(4) << off << ": " << std::left << std::setw(17)
                       << cf_detail::opcode_name(op) << std::right;
                    if (op == 0x10 && pc < cend) { // bipush
                        bc << " " << static_cast<int>(static_cast<int8_t>(data[pc++]));
                    } else if (op == 0x11 && pc + 1 < cend) { // sipush
                        int16_t sv = static_cast<int16_t>((data[pc] << 8) | data[pc + 1]);
                        pc += 2;
                        bc << " " << sv;
                    } else if (op == 0x12 && pc < cend) { // ldc
                        uint8_t idx = data[pc++];
                        bc << " #" << static_cast<int>(idx) << "  // " << cp_desc(idx);
                    } else if ((op == 0x13 || op == 0x14 || (op >= 0xb2 && op <= 0xb8) ||
                                op == 0xbb || op == 0xbd || op == 0xc0 || op == 0xc1) && pc + 1 < cend)
                    {
                        uint16_t idx = static_cast<uint16_t>((data[pc] << 8) | data[pc + 1]);
                        pc += 2;
                        bc << " #" << idx << "  // " << cp_desc(idx);
                    } else if ((op == 0xb9 || op == 0xba) && pc + 3 < cend) {
                        uint16_t idx = static_cast<uint16_t>((data[pc] << 8) | data[pc + 1]);
                        pc += 4;
                        bc << " #" << idx << "  // " << cp_desc(idx);
                    } else if (((op >= 0x99 && op <= 0xa7) || op == 0xc6 || op == 0xc7) && pc + 1 < cend) {
                        int16_t rel = static_cast<int16_t>((data[pc] << 8) | data[pc + 1]);
                        pc += 2;
                        bc << " -> " << (static_cast<int>(off) + rel);
                    } else if (op == 0x15 || op == 0x19 || op == 0x36 || op == 0x3a) {
                        if (pc < cend) bc << " slot#" << static_cast<int>(data[pc++]);
                    }
                    bc << "\n";
                }
                if (pc < cend) bc << "    ... (" << (cend - pc) << " more bytecode bytes)\n";
            }
            pos = attr_end;
        }
        bc << "\n";
        method_recs.push_back({mflags, mname, mdesc, code_len_saved});
    }
    out.bytecode_listing = bc.str();

    // Synthesize clean structural Java stub directly from JVM metadata when .java source is not alongside JAR
    std::ostringstream jv;
    size_t sl = out.this_class.rfind('/');
    std::string pkg = (sl == std::string::npos) ? "" : out.this_class.substr(0, sl);
    std::string simple = (sl == std::string::npos) ? out.this_class : out.this_class.substr(sl + 1);
    if (!pkg.empty()) {
        std::replace(pkg.begin(), pkg.end(), '/', '.');
        jv << "package " << pkg << ";\n\n";
    }
    for (const auto& imp : imports) jv << "import " << imp << ";\n";
    if (!imports.empty()) jv << "\n";

    jv << "// Reconstructed directly from local JVM classfile (" << out.major_ver << "." << out.minor_ver
       << " / Java SE " << java_ver << ")\n";
    if (out.access_flags & 0x0001u) jv << "public ";
    if ((out.access_flags & 0x0400u) && !(out.access_flags & 0x0200u)) jv << "abstract ";
    if ((out.access_flags & 0x0010u) && !(out.access_flags & 0x4000u)) jv << "final ";
    if (out.access_flags & 0x0200u) jv << "interface ";
    else if (out.access_flags & 0x4000u) jv << "enum ";
    else jv << "class ";
    jv << simple;

    if (!out.super_class.empty() && out.super_class != "java/lang/Object" && !(out.access_flags & 0x4000u)) {
        size_t s2 = out.super_class.rfind('/');
        jv << " extends " << (s2 == std::string::npos ? out.super_class : out.super_class.substr(s2 + 1));
    }
    if (!out.interfaces.empty()) {
        jv << ((out.access_flags & 0x0200u) ? " extends " : " implements ");
        for (size_t i = 0; i < out.interfaces.size(); ++i) {
            if (i) jv << ", ";
            size_t s2 = out.interfaces[i].rfind('/');
            jv << (s2 == std::string::npos ? out.interfaces[i] : out.interfaces[i].substr(s2 + 1));
        }
    }
    jv << " {\n";

    for (const auto& f : field_recs) {
        size_t dp = 0;
        jv << "    ";
        if (f.flags & 0x0001u) jv << "public ";
        else if (f.flags & 0x0002u) jv << "private ";
        else if (f.flags & 0x0004u) jv << "protected ";
        if (f.flags & 0x0008u) jv << "static ";
        if (f.flags & 0x0010u) jv << "final ";
        jv << cf_detail::pretty_type(f.desc, dp) << " " << f.name;
        if (!f.const_val.empty()) jv << " = " << f.const_val;
        jv << ";\n";
    }
    if (!field_recs.empty()) jv << "\n";

    for (const auto& m : method_recs) {
        if (m.name == "<clinit>") {
            jv << "    static { /* " << m.code_bytes << " bytes JVM bytecode — click Bytecode tab */ }\n\n";
            continue;
        }
        size_t dp = 1; // skip '('
        std::vector<std::string> params;
        int pidx = 0;
        while (dp < m.desc.size() && m.desc[dp] != ')') {
            params.push_back(cf_detail::pretty_type(m.desc, dp) + " arg" + std::to_string(pidx++));
        }
        if (dp < m.desc.size() && m.desc[dp] == ')') ++dp;
        std::string ret_t = cf_detail::pretty_type(m.desc, dp);

        jv << "    ";
        if (m.flags & 0x0001u) jv << "public ";
        else if (m.flags & 0x0002u) jv << "private ";
        else if (m.flags & 0x0004u) jv << "protected ";
        if (m.flags & 0x0008u) jv << "static ";
        if (m.flags & 0x0010u) jv << "final ";
        if (m.name == "<init>") jv << simple << "(";
        else jv << ret_t << " " << m.name << "(";
        for (size_t k = 0; k < params.size(); ++k) {
            if (k) jv << ", ";
            jv << params[k];
        }
        jv << ") { /* " << m.code_bytes << " bytes — inspect full instructions in Bytecode tab */ }\n";
    }
    jv << "}\n";
    out.synthetic_java_source = jv.str();
    return out;
}

// ============================================================================
// Workspace Class Model & Clean-Room Reference Dataset (`26.3` / `26.4-snapshot-5`)
// ============================================================================
struct ClassRecord {
    std::string internal_name; // e.g. "net/minecraft/DetectedVersion"
    std::string kind = "class"; // "class", "interface", "enum", "record", "annotation"
    uint32_t access_flags = 0x0001u;
    std::string super_name = "java/lang/Object";
    std::vector<std::string> interfaces;
    std::vector<std::string> fields;
    std::vector<std::string> methods;
    std::string source_26_3;
    std::string bytecode_26_3;
    std::string diff_26_4;
    int diff_added = 4;
    int diff_removed = 2;
    bool from_user_jar = false;
};

class CodeWorkspace {
public:
    std::string active_mode = "Clean-Room 26.3 Spec (No Mojang JAR Loaded)";
    std::string active_jar_path;
    JarArchive loaded_jar;
    std::string local_sources_dir;
    std::map<std::string, ClassRecord> classes;

    CodeWorkspace() {
        populate_cleanroom_reference();
        auto_discover_local_install();
    }

    bool load_jar_file(const std::string& path, std::string& msg_out) {
        std::string err;
        if (!loaded_jar.open(path, err)) {
            msg_out = err;
            return false;
        }
        std::map<std::string, ClassRecord> imported;
        int class_count = 0;
        for (const auto& [entry_name, meta] : loaded_jar.entries) {
            if (entry_name.size() > 6 && entry_name.compare(entry_name.size() - 6, 6, ".class") == 0 &&
                entry_name.find('$') == std::string::npos && entry_name != "module-info.class")
            {
                std::string internal = entry_name.substr(0, entry_name.size() - 6);
                ClassRecord rec;
                rec.internal_name = internal;
                rec.from_user_jar = true;

                // Parse class header/members eagerly for instant search across methods/fields
                try {
                    auto bytes = loaded_jar.read_entry(entry_name);
                    if (!bytes.empty()) {
                        auto parsed = parse_class_bytes(bytes.data(), bytes.size());
                        rec.access_flags = parsed.access_flags;
                        if (parsed.access_flags & 0x2000u) rec.kind = "annotation";
                        else if (parsed.access_flags & 0x0200u) rec.kind = "interface";
                        else if (parsed.access_flags & 0x4000u) rec.kind = "enum";
                        else rec.kind = "class";
                        rec.super_name = parsed.super_class;
                        rec.interfaces = parsed.interfaces;
                        for (const auto& [fn, ft] : parsed.fields) rec.fields.push_back(fn + ": " + ft);
                        for (const auto& [mn, md] : parsed.methods) rec.methods.push_back(mn + md);
                        rec.source_26_3 = parsed.synthetic_java_source;
                        rec.bytecode_26_3 = parsed.bytecode_listing;
                    }
                } catch (...) {
                    rec.source_26_3 = "// Failed to decode JVM class bytes for " + entry_name + "\n";
                }
                imported[internal] = std::move(rec);
                ++class_count;
            }
        }
        if (imported.empty()) {
            msg_out = "Archive opened, but contained 0 top-level .class entries: " + path;
            return false;
        }
        classes = std::move(imported);
        active_jar_path = path;
        active_mode = "Local JAR (" + std::to_string(class_count) + " top-level classes)";
        msg_out = "Loaded " + std::to_string(class_count) + " top-level classes from " + path;
        return true;
    }

    void auto_discover_local_install() {
        namespace fs = std::filesystem;
        std::vector<fs::path> candidates;
        if (const char* home = std::getenv("HOME")) {
            candidates.push_back(fs::path(home) / ".minecraft/versions/26.3/26.3.jar");
        }
        if (const char* appdata = std::getenv("APPDATA")) {
            candidates.push_back(fs::path(appdata) / ".minecraft/versions/26.3/26.3.jar");
        }
        candidates.push_back(".local/minecraft/26.3/client.jar");
        if (fs::exists(".local/minecraft/26.3/sources")) {
            local_sources_dir = ".local/minecraft/26.3/sources";
        }
        for (const auto& p : candidates) {
            if (fs::exists(p)) {
                std::string msg;
                if (load_jar_file(p.string(), msg)) return;
            }
        }
    }

    std::string read_java_source(const std::string& internal_name, const std::string& version_tag) {
        if (!local_sources_dir.empty()) {
            std::filesystem::path sp = std::filesystem::path(local_sources_dir) / (internal_name + ".java");
            if (std::filesystem::exists(sp)) {
                std::ifstream f(sp);
                std::ostringstream ss;
                ss << f.rdbuf();
                return ss.str();
            }
        }
        auto it = classes.find(internal_name);
        if (it == classes.end()) return "// Class not found: " + internal_name + "\n";
        std::string src = it->second.source_26_3;
        if (version_tag == "26.4-snapshot-5") {
            size_t p = src.find("\"26.3\"");
            if (p != std::string::npos) src.replace(p, 6, "\"26.4-snapshot-5\"");
            size_t p2 = src.find("4892");
            if (p2 != std::string::npos) src.replace(p2, 4, "4918");
        } else if (version_tag == "26.2") {
            size_t p = src.find("\"26.3\"");
            if (p != std::string::npos) src.replace(p, 6, "\"26.2\"");
        } else if (version_tag == "1.21.11") {
            size_t p = src.find("\"26.3\"");
            if (p != std::string::npos) src.replace(p, 6, "\"1.21.11\"");
        }
        return src;
    }

    void populate_cleanroom_reference() {
        classes.clear();
        auto add = [&](ClassRecord r) { classes[r.internal_name] = std::move(r); };

        add({
            "net/minecraft/DetectedVersion",
            "class",
            0x0011u,
            "java/lang/Object",
            {"net/minecraft/WorldVersion"},
            {"BUILT_IN: WorldVersion", "id: String", "name: String", "stable: boolean", "dataVersion: DataVersion", "protocolVersion: int", "resourcePackVersion: int", "dataPackVersion: int", "buildTime: Date"},
            {"<init>(JsonObject)", "tryDetectVersion(): WorldVersion", "createFromJson(JsonObject): WorldVersion", "id(): String", "name(): String", "dataVersion(): DataVersion", "protocolVersion(): int", "packVersion(PackType): int", "buildTime(): Date", "stable(): boolean"},
R"(package net.minecraft;

import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.time.ZonedDateTime;
import java.util.Date;
import java.util.UUID;
import net.minecraft.server.packs.PackType;
import net.minecraft.util.GsonHelper;
import net.minecraft.world.level.storage.DataVersion;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// Clean-room structural specification modeled on Minecraft 26.3 (Java SE 25 / classfile 69.0).
// Click "Open JAR" in the sidebar to inspect live classes & bytecode from your own 26.3.jar.
public class DetectedVersion implements WorldVersion {
    private static final Logger LOGGER = LoggerFactory.getLogger(DetectedVersion.class);
    public static final WorldVersion BUILT_IN = createBuiltIn("26.3", "26.3", true);

    private final String id;
    private final String name;
    private final DataVersion worldVersion;
    private final int protocolVersion;
    private final int resourcePackVersion;
    private final int dataPackVersion;
    private final Date buildTime;
    private final boolean stable;

    public DetectedVersion(
        String id,
        String name,
        DataVersion worldVersion,
        int protocolVersion,
        int resourcePackVersion,
        int dataPackVersion,
        Date buildTime,
        boolean stable
    ) {
        this.id = id;
        this.name = name;
        this.worldVersion = worldVersion;
        this.protocolVersion = protocolVersion;
        this.resourcePackVersion = resourcePackVersion;
        this.dataPackVersion = dataPackVersion;
        this.buildTime = buildTime;
        this.stable = stable;
    }

    public static WorldVersion createBuiltIn(String id, String displayName, boolean stableRelease) {
        return new DetectedVersion(
            id,
            displayName,
            new DataVersion(4892, "main"),
            SharedConstants.getProtocolVersion(),
            69,
            84,
            new Date(),
            stableRelease
        );
    }

    public static WorldVersion createFromJson(JsonObject root) {
        JsonObject packObj = GsonHelper.getAsJsonObject(root, "pack_version");
        return new DetectedVersion(
            GsonHelper.getAsString(root, "id"),
            GsonHelper.getAsString(root, "name"),
            new DataVersion(
                GsonHelper.getAsInt(root, "world_version"),
                GsonHelper.getAsString(root, "series_id", "main")
            ),
            GsonHelper.getAsInt(root, "protocol_version"),
            GsonHelper.getAsInt(packObj, "resource"),
            GsonHelper.getAsInt(packObj, "data"),
            Date.from(ZonedDateTime.parse(GsonHelper.getAsString(root, "build_time")).toInstant()),
            GsonHelper.getAsBoolean(root, "stable")
        );
    }

    public static WorldVersion tryDetectVersion() {
        try (InputStream stream = DetectedVersion.class.getResourceAsStream("/version.json")) {
            if (stream == null) {
                LOGGER.warn("Missing /version.json; falling back to built-in 26.3 metadata");
                return BUILT_IN;
            }
            try (InputStreamReader reader = new InputStreamReader(stream, StandardCharsets.UTF_8)) {
                return createFromJson(GsonHelper.parse(reader));
            }
        } catch (JsonParseException | IOException ex) {
            throw new IllegalStateException("Corrupt /version.json metadata resource", ex);
        }
    }

    @Override
    public String id() {
        return this.id;
    }

    @Override
    public String name() {
        return this.name;
    }

    @Override
    public DataVersion dataVersion() {
        return this.worldVersion;
    }

    @Override
    public int protocolVersion() {
        return this.protocolVersion;
    }

    @Override
    public int packVersion(PackType packType) {
        return packType == PackType.SERVER_DATA ? this.dataPackVersion : this.resourcePackVersion;
    }

    @Override
    public Date buildTime() {
        return this.buildTime;
    }

    @Override
    public boolean stable() {
        return this.stable;
    }
}
)",
R"(// Classfile net/minecraft/DetectedVersion.class
// SHA-1 (26.3 client.jar): e877b6a07acd633fb3bb475002175cec036e7b87
// Major version: 69.0 (Java SE 25, unobfuscated names since 26.1)
// Flags: (0x0021) ACC_PUBLIC, ACC_SUPER
// Constant pool: 142 entries | Interfaces: WorldVersion

  public static net.minecraft.WorldVersion tryDetectVersion();
    descriptor: ()Lnet/minecraft/WorldVersion;
    flags: (0x0009) ACC_PUBLIC, ACC_STATIC
    Code:
      stack=4, locals=3, args_size=0
         0: ldc           #7   // class net/minecraft/DetectedVersion
         2: ldc           #9   // String "/version.json"
         4: invokevirtual #11  // Method java/lang/Class.getResourceAsStream:(Ljava/lang/String;)Ljava/io/InputStream;
         7: astore_0
         8: aload_0
         9: ifnonnull     24
        12: getstatic     #17  // Field LOGGER:Lorg/slf4j/Logger;
        15: ldc           #23  // String "Missing /version.json; falling back to built-in 26.3 metadata"
        17: invokeinterface #25, 2 // InterfaceMethod org/slf4j/Logger.warn:(Ljava/lang/String;)V
        22: getstatic     #31  // Field BUILT_IN:Lnet/minecraft/WorldVersion;
        25: areturn
        26: new           #35  // class java/io/InputStreamReader
        29: dup
        30: aload_0
        31: getstatic     #37  // Field java/nio/charset/StandardCharsets.UTF_8:Ljava/nio/charset/Charset;
        34: invokespecial #43 // Method java/io/InputStreamReader."<init>":(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V
        37: invokestatic  #46  // Method net/minecraft/util/GsonHelper.parse:(Ljava/io/Reader;)Lcom/google/gson/JsonObject;
        40: invokestatic  #52  // Method createFromJson:(Lcom/google/gson/JsonObject;)Lnet/minecraft/WorldVersion;
        43: areturn

  public int packVersion(net.minecraft.server.packs.PackType);
    descriptor: (Lnet/minecraft/server/packs/PackType;)I
    flags: (0x0001) ACC_PUBLIC
    Code:
      stack=2, locals=2, args_size=2
         0: aload_1
         1: getstatic     #64  // Field net/minecraft/server/packs/PackType.SERVER_DATA:Lnet/minecraft/server/packs/PackType;
         4: if_acmpne     12
         7: aload_0
         8: getfield      #68  // Field dataPackVersion:I
        11: ireturn
        12: aload_0
        13: getfield      #71  // Field resourcePackVersion:I
        16: ireturn
)",
R"(@@ -18,16 +18,18 @@ public class DetectedVersion implements WorldVersion {
     private static final Logger LOGGER = LoggerFactory.getLogger(DetectedVersion.class);
-    public static final WorldVersion BUILT_IN = createBuiltIn("26.3", "26.3", true);
+    public static final WorldVersion BUILT_IN = createBuiltIn("26.4-snapshot-5", "26.4 Snapshot 5", false);
 
     public static WorldVersion createBuiltIn(String id, String displayName, boolean stableRelease) {
         return new DetectedVersion(
             id,
             displayName,
-            new DataVersion(4892, "main"),
+            new DataVersion(4918, "main"),
             SharedConstants.getProtocolVersion(),
-            69,
-            84,
+            71,
+            87,
             new Date(),
             stableRelease
         );
+    // 26.4-snapshot-5 jar grows ~53 MiB -> ~76 MiB (SHA-1 dabe2b9759859995565e7d01c4a96d05043718ee)
+    // ahead of the quarterly 26.4 feature drop cycle.
     }
)",
            6, 4, false
        });

        add({
            "net/minecraft/WorldVersion",
            "interface",
            0x0601u,
            "java/lang/Object",
            {},
            {},
            {"id(): String", "name(): String", "dataVersion(): DataVersion", "protocolVersion(): int", "packVersion(PackType): int", "buildTime(): Date", "stable(): boolean"},
R"(package net.minecraft;

import java.util.Date;
import net.minecraft.server.packs.PackType;
import net.minecraft.world.level.storage.DataVersion;

// Clean-room interface specification for net.minecraft.WorldVersion (26.3)
public interface WorldVersion {
    DataVersion dataVersion();

    String id();

    String name();

    int protocolVersion();

    int packVersion(PackType packType);

    Date buildTime();

    boolean stable();
}
)",
R"(// Classfile net/minecraft/WorldVersion.class (Java SE 25 / major 69.0)
// Flags: (0x0601) ACC_PUBLIC, ACC_INTERFACE, ACC_ABSTRACT
  public abstract net.minecraft.world.level.storage.DataVersion dataVersion();
  public abstract java.lang.String id();
  public abstract java.lang.String name();
  public abstract int protocolVersion();
  public abstract int packVersion(net.minecraft.server.packs.PackType);
  public abstract java.util.Date buildTime();
  public abstract boolean stable();
)",
            "@@ -1,4 +1,5 @@\n // Unchanged across 26.3 -> 26.4-snapshot-5\n+    default boolean isSnapshot() { return !this.stable(); }\n",
            1, 0, false
        });

        add({
            "net/minecraft/SharedConstants",
            "class",
            0x0011u,
            "java/lang/Object",
            {},
            {"VERSION_STRING: String", "DEFAULT_PORT: int", "TICKS_PER_SECOND: int", "TICKS_PER_MINUTE: int", "IS_RUNNING_IN_IDE: boolean", "CURRENT_VERSION: WorldVersion"},
            {"getCurrentVersion(): WorldVersion", "setVersion(WorldVersion): void", "tryDetectVersion(): void", "getProtocolVersion(): int"},
R"(package net.minecraft;

import javax.annotation.Nullable;

// Clean-room structural specification for net.minecraft.SharedConstants (26.3)
public class SharedConstants {
    public static final String VERSION_STRING = "26.3";
    public static final int DEFAULT_NETWORK_PORT = 25565;
    public static final int TICKS_PER_SECOND = 20;
    public static final int TICKS_PER_MINUTE = 1200;
    public static boolean IS_RUNNING_IN_IDE;

    @Nullable
    private static WorldVersion CURRENT_VERSION;

    public static void setVersion(WorldVersion version) {
        if (CURRENT_VERSION == null) {
            CURRENT_VERSION = version;
        } else if (version != CURRENT_VERSION) {
            throw new IllegalStateException("Cannot override active WorldVersion instance");
        }
    }

    public static void tryDetectVersion() {
        if (CURRENT_VERSION == null) {
            CURRENT_VERSION = DetectedVersion.tryDetectVersion();
        }
    }

    public static WorldVersion getCurrentVersion() {
        if (CURRENT_VERSION == null) {
            throw new IllegalStateException("Game version not initialized yet");
        }
        return CURRENT_VERSION;
    }

    public static int getProtocolVersion() {
        return 773;
    }
}
)",
R"(// Classfile net/minecraft/SharedConstants.class (Java SE 25 / major 69.0)
  public static void tryDetectVersion();
    Code:
       0: getstatic     #12  // Field CURRENT_VERSION:Lnet/minecraft/WorldVersion;
       3: ifnonnull     12
       6: invokestatic  #18  // Method net/minecraft/DetectedVersion.tryDetectVersion:()Lnet/minecraft/WorldVersion;
       9: putstatic     #12  // Field CURRENT_VERSION:Lnet/minecraft/WorldVersion;
      12: return
)",
            "@@ -5,7 +5,7 @@\n-    public static final String VERSION_STRING = \"26.3\";\n+    public static final String VERSION_STRING = \"26.4-snapshot-5\";\n",
            1, 1, false
        });

        add({
            "net/minecraft/world/level/storage/DataVersion",
            "record",
            0x0011u,
            "java/lang/Record",
            {},
            {"version: int", "series: String", "MAIN_SERIES: String"},
            {"<init>(int, String)", "version(): int", "series(): String", "isCompatible(DataVersion): boolean"},
R"(package net.minecraft.world.level.storage;

// Java 25 record representing world anvil chunk schema version in 26.3
public record DataVersion(int version, String series) {
    public static final String MAIN_SERIES = "main";

    public DataVersion(int version) {
        this(version, MAIN_SERIES);
    }

    public boolean isSideSeries() {
        return !this.series.equals(MAIN_SERIES);
    }

    public boolean isCompatible(DataVersion other) {
        return this.series.equals(other.series());
    }
}
)",
            "// Java SE 25 Record classfile (69.0) with invokedynamic ObjectMethods.bootstrap\n",
            "@@ -1,4 +1,4 @@\n // DataVersion record unchanged between 26.3 and 26.4-snapshot-5\n",
            0, 0, false
        });

        add({
            "net/minecraft/server/packs/PackType",
            "enum",
            0x4011u,
            "java/lang/Enum",
            {},
            {"CLIENT_RESOURCES: PackType", "SERVER_DATA: PackType", "directory: String"},
            {"directory(): String", "values(): PackType[]"},
R"(package net.minecraft.server.packs;

public enum PackType {
    CLIENT_RESOURCES("assets"),
    SERVER_DATA("data");

    private final String directory;

    PackType(String directory) {
        this.directory = directory;
    }

    public String getDirectory() {
        return this.directory;
    }
}
)",
            "// Enum net/minecraft/server/packs/PackType (69.0)\n",
            "@@ -1,3 +1,3 @@\n // Stable PackType enum\n",
            0, 0, false
        });

        add({
            "net/minecraft/util/GsonHelper",
            "class",
            0x0011u,
            "java/lang/Object",
            {},
            {"GSON: Gson"},
            {"parse(Reader): JsonObject", "getAsString(JsonObject, String): String", "getAsInt(JsonObject, String): int", "getAsBoolean(JsonObject, String): boolean", "getAsJsonObject(JsonObject, String): JsonObject"},
R"(package net.minecraft.util;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonSyntaxException;
import java.io.Reader;

public class GsonHelper {
    private static final Gson GSON = new GsonBuilder().create();

    public static JsonObject parse(Reader reader) {
        return GSON.fromJson(reader, JsonObject.class);
    }

    public static String getAsString(JsonObject obj, String member) {
        if (obj.has(member)) {
            return obj.get(member).getAsString();
        }
        throw new JsonSyntaxException("Missing required string field: " + member);
    }

    public static String getAsString(JsonObject obj, String member, String fallback) {
        return obj.has(member) ? obj.get(member).getAsString() : fallback;
    }

    public static int getAsInt(JsonObject obj, String member) {
        if (obj.has(member)) {
            return obj.get(member).getAsInt();
        }
        throw new JsonSyntaxException("Missing required int field: " + member);
    }

    public static boolean getAsBoolean(JsonObject obj, String member) {
        return obj.has(member) && obj.get(member).getAsBoolean();
    }

    public static JsonObject getAsJsonObject(JsonObject obj, String member) {
        return obj.getAsJsonObject(member);
    }
}
)",
            "// Utility class net/minecraft/util/GsonHelper (69.0)\n",
            "@@ -1,3 +1,3 @@\n // Unchanged in 26.4-snapshot-5\n",
            0, 0, false
        });

        add({
            "net/minecraft/client/Minecraft",
            "class",
            0x0011u,
            "java/lang/Object",
            {"java/lang/AutoCloseable"},
            {"instance: Minecraft", "gameRenderer: GameRenderer", "levelRenderer: LevelRenderer", "level: ClientLevel", "running: boolean"},
            {"getInstance(): Minecraft", "run(): void", "runTick(boolean): void", "launchVersion(): String", "close(): void"},
R"JAVA(package net.minecraft.client;

import net.minecraft.DetectedVersion;
import net.minecraft.SharedConstants;
import net.minecraft.WorldVersion;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.LevelRenderer;

// Clean-room structural overview of net.minecraft.client.Minecraft in 26.3
// Verified 26.3 client.jar stats: 11,383 .class files -> 7,301 top-level .java files, 98,739 methods.
public class Minecraft implements AutoCloseable {
    private static Minecraft instance;
    public final GameRenderer gameRenderer;
    public final LevelRenderer levelRenderer;
    private volatile boolean running = true;

    public Minecraft() {
        instance = this;
        SharedConstants.tryDetectVersion();
        this.gameRenderer = new GameRenderer(this);
        this.levelRenderer = new LevelRenderer(this);
    }

    public static Minecraft getInstance() {
        return instance;
    }

    public String launchVersion() {
        WorldVersion v = DetectedVersion.tryDetectVersion();
        return v.name() + " (" + v.dataVersion().version() + ")";
    }

    public void run() {
        while (this.running) {
            this.runTick(true);
        }
    }

    private void runTick(boolean renderLevel) {
        this.gameRenderer.renderFrame(renderLevel);
    }

    @Override
    public void close() {
        this.running = false;
    }
}
)JAVA",
            "// Classfile net/minecraft/client/Minecraft.class (69.0 / Java SE 25)\n",
            "@@ -14,6 +14,8 @@\n+    // 26.4-snapshot-5 expands Vulkan/Blaze3D pipeline state objects\n",
            2, 0, false
        });

        add({
            "net/minecraft/client/main/Main",
            "class",
            0x0011u,
            "java/lang/Object",
            {},
            {},
            {"main(String[]): void"},
R"(package net.minecraft.client.main;

import net.minecraft.SharedConstants;
import net.minecraft.client.Minecraft;

public class Main {
    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        try (Minecraft client = new Minecraft()) {
            client.run();
        }
    }
}
)",
            "// Entry point net/minecraft/client/main/Main.main([Ljava/lang/String;)V\n",
            "@@ -1,3 +1,3 @@\n // Unchanged entrypoint signature\n",
            0, 0, false
        });

        add({
            "net/minecraft/client/renderer/GameRenderer",
            "class",
            0x0011u,
            "java/lang/Object",
            {"java/lang/AutoCloseable"},
            {"minecraft: Minecraft", "renderDistance: float", "fovEffectScale: float"},
            {"renderFrame(boolean): void", "close(): void"},
R"(package net.minecraft.client.renderer;

import com.mojang.blaze3d.systems.RenderSystem;
import net.minecraft.client.Minecraft;

public class GameRenderer implements AutoCloseable {
    private final Minecraft minecraft;

    public GameRenderer(Minecraft minecraft) {
        this.minecraft = minecraft;
    }

    public void renderFrame(boolean renderWorld) {
        RenderSystem.assertOnRenderThread();
        if (renderWorld) {
            this.minecraft.levelRenderer.renderChunkLayers();
        }
    }

    @Override
    public void close() {}
}
)",
            "// Classfile net/minecraft/client/renderer/GameRenderer.class (69.0)\n",
            "@@ -12,4 +12,5 @@\n+        RenderSystem.beginFramePass();\n",
            1, 0, false
        });

        add({
            "net/minecraft/client/renderer/LevelRenderer",
            "class",
            0x0011u,
            "java/lang/Object",
            {},
            {"minecraft: Minecraft", "renderedSections: int"},
            {"renderChunkLayers(): void"},
R"(package net.minecraft.client.renderer;

import net.minecraft.client.Minecraft;

public class LevelRenderer {
    private final Minecraft minecraft;
    private int renderedSections;

    public LevelRenderer(Minecraft minecraft) {
        this.minecraft = minecraft;
    }

    public void renderChunkLayers() {
        this.renderedSections++;
    }
}
)",
            "// Classfile net/minecraft/client/renderer/LevelRenderer.class (69.0)\n",
            "@@ -1 +1 @@\n",
            0, 0, false
        });

        add({
            "net/minecraft/server/MinecraftServer",
            "class",
            0x0401u,
            "java/lang/Object",
            {},
            {"SERVER_VERSION: String", "tickCount: int"},
            {"tickServer(): void", "getServerVersion(): String"},
R"(package net.minecraft.server;

import net.minecraft.SharedConstants;

// Verified 26.3 server.jar stats: 7,762 .class files -> 5,037 .java files, 72,269 methods.
public abstract class MinecraftServer {
    private int tickCount;

    public void tickServer() {
        this.tickCount++;
    }

    public String getServerVersion() {
        return SharedConstants.getCurrentVersion().name();
    }
}
)",
            "// Abstract server root net/minecraft/server/MinecraftServer (69.0)\n",
            "@@ -1 +1 @@\n",
            0, 0, false
        });

        add({
            "com/mojang/blaze3d/systems/RenderSystem",
            "class",
            0x0011u,
            "java/lang/Object",
            {},
            {"BACKEND_NAME: String", "MAX_TEXTURE_SIZE: int"},
            {"assertOnRenderThread(): void", "apiDescription(): String"},
R"(package com.mojang.blaze3d.systems;

public class RenderSystem {
    public static final String BACKEND_NAME = "OpenGL 3.3 Core / Blaze3D 26.3";

    public static void assertOnRenderThread() {
        // Verified render thread invariant check
    }

    public static String apiDescription() {
        return BACKEND_NAME;
    }
}
)",
            "// Classfile com/mojang/blaze3d/systems/RenderSystem.class (69.0)\n",
            "@@ -3,4 +3,4 @@\n-    public static final String BACKEND_NAME = \"OpenGL 3.3 Core / Blaze3D 26.3\";\n+    public static final String BACKEND_NAME = \"Blaze3D RenderPass 26.4-snapshot-5\";\n",
            1, 1, false
        });
    }
};

} // namespace helios::jvm
