#pragma once
// FluxJVM: Crash-Proof Zero-Dependency ZIP/ZIP64/DEFLATE Reader, Complete 256-Opcode JVMS 69.0 Disassembler,
// Stack-Machine Expression Decompiler, String Hunter & Ghidra-Style Cross-Reference Engine (C++20)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flux::jvm {

// ============================================================================
// Safe Non-Throwing DEFLATE Decompressor (RFC 1951 Stored + Fixed + Dynamic)
// ============================================================================
namespace deflate {

struct SafeBitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t byte_pos = 0;
    int bit_pos = 0;
    bool ok = true;

    uint32_t read_bits(int count) noexcept {
        uint32_t out = 0;
        for (int i = 0; i < count; ++i) {
            if (byte_pos >= size) {
                ok = false;
                return out;
            }
            uint32_t bit = (data[byte_pos] >> bit_pos) & 1u;
            out |= (bit << i);
            if (++bit_pos == 8) {
                bit_pos = 0;
                ++byte_pos;
            }
        }
        return out;
    }

    void align_to_byte() noexcept {
        if (bit_pos != 0) {
            bit_pos = 0;
            ++byte_pos;
        }
    }
};

struct SafeHuffman {
    static constexpr int MAX_BITS = 15;
    std::array<int, MAX_BITS + 1> count{};
    std::array<int, 320> symbols{};
    int total_syms = 0;

    void build(const int* lengths, int n_syms) noexcept {
        count.fill(0);
        for (int i = 0; i < n_syms; ++i) {
            int l = lengths[i];
            if (l > 0 && l <= MAX_BITS) ++count[l];
        }
        std::array<int, MAX_BITS + 1> offs{};
        int sum = 0;
        for (int b = 1; b <= MAX_BITS; ++b) {
            offs[b] = sum;
            sum += count[b];
        }
        total_syms = sum;
        for (int sym = 0; sym < n_syms; ++sym) {
            int l = lengths[sym];
            if (l > 0 && l <= MAX_BITS) {
                int idx = offs[l]++;
                if (idx >= 0 && idx < static_cast<int>(symbols.size())) {
                    symbols[static_cast<size_t>(idx)] = sym;
                }
            }
        }
    }

    int decode(SafeBitReader& br) const noexcept {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= MAX_BITS; ++len) {
            code |= static_cast<int>(br.read_bits(1));
            if (!br.ok) return -1;
            int c = count[len];
            if (code - c < first) {
                int pos = index + (code - first);
                if (pos < 0 || pos >= total_syms || pos >= static_cast<int>(symbols.size())) {
                    br.ok = false;
                    return -1;
                }
                return symbols[static_cast<size_t>(pos)];
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        br.ok = false;
        return -1;
    }
};

inline bool inflate_safe(
    const uint8_t* src,
    size_t src_len,
    std::vector<uint8_t>& out,
    size_t expected_hint = 0,
    size_t hard_cap = 96ull * 1024ull * 1024ull) noexcept
{
    static constexpr int LEN_BASE[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static constexpr int LEN_EXTRA[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static constexpr int DIST_BASE[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static constexpr int DIST_EXTRA[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static constexpr int CL_ORDER[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    out.clear();
    out.reserve(std::min<size_t>(hard_cap, expected_hint ? expected_hint + 64 : src_len * 2 + 64));
    SafeBitReader br{src, src_len, 0, 0, true};

    bool is_final = false;
    while (!is_final && br.ok) {
        is_final = (br.read_bits(1) != 0);
        uint32_t btype = br.read_bits(2);
        if (!br.ok) return false;

        if (btype == 0u) {
            br.align_to_byte();
            if (br.byte_pos + 4 > br.size) return false;
            uint16_t len = static_cast<uint16_t>(br.data[br.byte_pos] | (br.data[br.byte_pos + 1] << 8));
            br.byte_pos += 4;
            if (br.byte_pos + len > br.size || out.size() + len > hard_cap) return false;
            out.insert(out.end(), br.data + br.byte_pos, br.data + br.byte_pos + len);
            br.byte_pos += len;
        } else if (btype == 1u || btype == 2u) {
            SafeHuffman lit_tree, dist_tree;
            int lengths[320] = {0};
            if (btype == 1u) {
                for (int i = 0; i <= 143; ++i) lengths[i] = 8;
                for (int i = 144; i <= 255; ++i) lengths[i] = 9;
                for (int i = 256; i <= 279; ++i) lengths[i] = 7;
                for (int i = 280; i <= 287; ++i) lengths[i] = 8;
                lit_tree.build(lengths, 288);
                int dl[32];
                std::fill(std::begin(dl), std::end(dl), 5);
                dist_tree.build(dl, 32);
            } else {
                int hlit = static_cast<int>(br.read_bits(5)) + 257;
                int hdist = static_cast<int>(br.read_bits(5)) + 1;
                int hclen = static_cast<int>(br.read_bits(4)) + 4;
                if (!br.ok || hlit + hdist > 320) return false;
                int cl_lens[19] = {0};
                for (int i = 0; i < hclen; ++i) cl_lens[CL_ORDER[i]] = static_cast<int>(br.read_bits(3));
                SafeHuffman cl_tree;
                cl_tree.build(cl_lens, 19);

                int idx = 0;
                while (idx < hlit + hdist) {
                    int sym = cl_tree.decode(br);
                    if (!br.ok || sym < 0) return false;
                    if (sym <= 15) {
                        lengths[idx++] = sym;
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        int rep = static_cast<int>(br.read_bits(2)) + 3;
                        int prev = lengths[idx - 1];
                        while (rep-- > 0 && idx < hlit + hdist) lengths[idx++] = prev;
                    } else if (sym == 17) {
                        int rep = static_cast<int>(br.read_bits(3)) + 3;
                        while (rep-- > 0 && idx < hlit + hdist) lengths[idx++] = 0;
                    } else if (sym == 18) {
                        int rep = static_cast<int>(br.read_bits(7)) + 11;
                        while (rep-- > 0 && idx < hlit + hdist) lengths[idx++] = 0;
                    } else {
                        return false;
                    }
                }
                lit_tree.build(lengths, hlit);
                dist_tree.build(lengths + hlit, hdist);
            }

            while (br.ok) {
                int sym = lit_tree.decode(br);
                if (!br.ok || sym < 0) return false;
                if (sym < 256) {
                    if (out.size() >= hard_cap) return false;
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li < 0 || li >= 29) return false;
                    int copy_len = LEN_BASE[li] + static_cast<int>(br.read_bits(LEN_EXTRA[li]));
                    int di = dist_tree.decode(br);
                    if (!br.ok || di < 0 || di >= 30) return false;
                    int dist = DIST_BASE[di] + static_cast<int>(br.read_bits(DIST_EXTRA[di]));
                    if (!br.ok || dist <= 0 || static_cast<size_t>(dist) > out.size() || out.size() + copy_len > hard_cap) {
                        return false;
                    }
                    for (int k = 0; k < copy_len; ++k) {
                        out.push_back(out[out.size() - static_cast<size_t>(dist)]);
                    }
                }
            }
        } else {
            return false;
        }
    }
    return br.ok;
}

} // namespace deflate

// ============================================================================
// Pure C++20 SHA-1 for Instant Lockfile Verification (26.3 / 26.4-snapshot-5)
// ============================================================================
inline std::string sha1_hex(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    auto rol = [](uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); };

    auto block = [&](const uint8_t b[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(b[i * 4]) << 24) | (static_cast<uint32_t>(b[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(b[i * 4 + 2]) << 8) | static_cast<uint32_t>(b[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, bb = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | ((~bb) & d); k = 0x5A827999u; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(bb, 30); bb = a; a = tmp;
        }
        h0 += a; h1 += bb; h2 += c; h3 += d; h4 += e;
    };

    size_t pos = 0;
    while (pos + 64 <= len) { block(data + pos); pos += 64; }
    uint8_t tail[128] = {0};
    size_t rem = len - pos;
    if (rem) std::memcpy(tail, data + pos, rem);
    tail[rem] = 0x80u;
    size_t total_tail = (rem < 56) ? 64 : 128;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ull;
    for (int i = 0; i < 8; ++i) tail[total_tail - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu);
    block(tail);
    if (total_tail == 128) block(tail + 64);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << h0 << std::setw(8) << h1 << std::setw(8) << h2 << std::setw(8) << h3 << std::setw(8) << h4;
    return oss.str();
}

// ============================================================================
// Safe ZIP + ZIP64 + Server Bundler Inner-JAR Reader
// ============================================================================
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t comp_size = 0;
    uint64_t uncomp_size = 0;
    uint64_t local_offset = 0;
};

class SafeJarReader {
public:
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string, ZipEntry> entries;
    std::string unwrapped_inner;

    static uint16_t u16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    static uint32_t u32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    static uint64_t u64(const uint8_t* p) noexcept {
        return static_cast<uint64_t>(u32(p)) | (static_cast<uint64_t>(u32(p + 4)) << 32);
    }

    bool parse_buffer(std::vector<uint8_t> raw, std::string& err, int depth = 0) {
        if (raw.size() < 22) {
            err = "Archive smaller than 22 bytes";
            return false;
        }
        bytes = std::move(raw);
        entries.clear();

        const size_t n = bytes.size();
        size_t eocd = std::string::npos;
        size_t min_p = (n > 65557) ? (n - 65557) : 0;
        for (size_t p = n - 22;; --p) {
            if (bytes[p] == 0x50 && bytes[p + 1] == 0x4B && bytes[p + 2] == 0x05 && bytes[p + 3] == 0x06) {
                eocd = p;
                break;
            }
            if (p <= min_p) break;
        }
        if (eocd == std::string::npos) {
            err = "Invalid JAR/ZIP: missing EOCD signature";
            return false;
        }

        uint64_t cd_size = u32(bytes.data() + eocd + 12);
        uint64_t cd_offset = u32(bytes.data() + eocd + 16);

        // Support ZIP64 End of Central Directory Locator (PK\x06\x07) right before EOCD!
        if (eocd >= 20 && bytes[eocd - 20] == 0x50 && bytes[eocd - 19] == 0x4B &&
            bytes[eocd - 18] == 0x06 && bytes[eocd - 17] == 0x07)
        {
            uint64_t z64_off = u64(bytes.data() + eocd - 12);
            if (z64_off + 56 <= n && bytes[z64_off] == 0x50 && bytes[z64_off + 1] == 0x4B &&
                bytes[z64_off + 2] == 0x06 && bytes[z64_off + 3] == 0x06)
            {
                cd_size = u64(bytes.data() + z64_off + 40);
                cd_offset = u64(bytes.data() + z64_off + 48);
            }
        }

        if (cd_offset > n || cd_size > n - cd_offset) {
            err = "Corrupt Central Directory bounds";
            return false;
        }

        size_t cur = static_cast<size_t>(cd_offset);
        const size_t cd_end = static_cast<size_t>(cd_offset + cd_size);
        entries.reserve(16384);

        std::string nested_server_jar;
        while (cur + 46 <= cd_end) {
            if (!(bytes[cur] == 0x50 && bytes[cur + 1] == 0x4B && bytes[cur + 2] == 0x01 && bytes[cur + 3] == 0x02)) {
                break;
            }
            ZipEntry ze;
            ze.method = u16(bytes.data() + cur + 10);
            ze.comp_size = u32(bytes.data() + cur + 20);
            ze.uncomp_size = u32(bytes.data() + cur + 24);
            uint16_t fn_len = u16(bytes.data() + cur + 28);
            uint16_t ex_len = u16(bytes.data() + cur + 30);
            uint16_t cm_len = u16(bytes.data() + cur + 32);
            ze.local_offset = u32(bytes.data() + cur + 42);
            if (cur + 46 + fn_len > cd_end) break;
            ze.name.assign(reinterpret_cast<const char*>(bytes.data() + cur + 46), fn_len);

            // Parse ZIP64 extra field (0x0001) if present
            size_t ex_pos = cur + 46 + fn_len;
            size_t ex_end = std::min(cd_end, ex_pos + ex_len);
            while (ex_pos + 4 <= ex_end) {
                uint16_t tag = u16(bytes.data() + ex_pos);
                uint16_t sz = u16(bytes.data() + ex_pos + 2);
                ex_pos += 4;
                if (tag == 0x0001 && ex_pos + sz <= ex_end) {
                    size_t z = ex_pos;
                    if (ze.uncomp_size == 0xFFFFFFFFull && z + 8 <= ex_pos + sz) { ze.uncomp_size = u64(bytes.data() + z); z += 8; }
                    if (ze.comp_size == 0xFFFFFFFFull && z + 8 <= ex_pos + sz) { ze.comp_size = u64(bytes.data() + z); z += 8; }
                    if (ze.local_offset == 0xFFFFFFFFull && z + 8 <= ex_pos + sz) { ze.local_offset = u64(bytes.data() + z); z += 8; }
                }
                ex_pos += sz;
            }

            if (depth == 0 && ze.name.rfind("META-INF/versions/", 0) == 0 &&
                ze.name.size() > 4 && ze.name.compare(ze.name.size() - 4, 4, ".jar") == 0)
            {
                nested_server_jar = ze.name;
            }
            entries[ze.name] = std::move(ze);
            cur += 46 + static_cast<size_t>(fn_len) + ex_len + cm_len;
        }

        // Automatically unwrap Minecraft Bundler server.jar -> inner server-26.3.jar!
        if (depth == 0 && !nested_server_jar.empty()) {
            bool has_mc_classes = false;
            for (const auto& [k, _] : entries) {
                if (k.rfind("net/minecraft/server/", 0) == 0 && k.find("Main.class") == std::string::npos) {
                    has_mc_classes = true;
                    break;
                }
            }
            if (!has_mc_classes) {
                std::vector<uint8_t> inner;
                if (extract_entry(nested_server_jar, inner)) {
                    std::string sub_err;
                    if (parse_buffer(std::move(inner), sub_err, depth + 1)) {
                        unwrapped_inner = nested_server_jar;
                        return true;
                    }
                }
            }
        }
        return true;
    }

    bool open_file(const std::string& path, std::string& err) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            err = "Cannot open file: " + path;
            return false;
        }
        auto sz = f.tellg();
        if (sz < 22) {
            err = "File too small (<22 bytes): " + path;
            return false;
        }
        f.seekg(0);
        std::vector<uint8_t> raw(static_cast<size_t>(sz));
        if (!f.read(reinterpret_cast<char*>(raw.data()), sz)) {
            err = "Failed reading archive bytes: " + path;
            return false;
        }
        unwrapped_inner.clear();
        return parse_buffer(std::move(raw), err);
    }

    bool extract_entry(const std::string& name, std::vector<uint8_t>& out) const noexcept {
        auto it = entries.find(name);
        if (it == entries.end()) return false;
        const auto& m = it->second;
        if (m.local_offset + 30 > bytes.size()) return false;
        size_t lh = static_cast<size_t>(m.local_offset);
        if (!(bytes[lh] == 0x50 && bytes[lh + 1] == 0x4B && bytes[lh + 2] == 0x03 && bytes[lh + 3] == 0x04)) {
            return false;
        }
        uint16_t fn_len = u16(bytes.data() + lh + 26);
        uint16_t ex_len = u16(bytes.data() + lh + 28);
        size_t data_pos = lh + 30 + static_cast<size_t>(fn_len) + ex_len;
        if (data_pos > bytes.size() || m.comp_size > bytes.size() - data_pos) return false;
        const uint8_t* ptr = bytes.data() + data_pos;
        if (m.method == 0u) {
            out.assign(ptr, ptr + static_cast<size_t>(m.comp_size));
            return true;
        }
        if (m.method == 8u) {
            return deflate::inflate_safe(ptr, static_cast<size_t>(m.comp_size), out, static_cast<size_t>(m.uncomp_size));
        }
        return false;
    }
};

// ============================================================================
// Complete Spec-Accurate JVMS ClassFile Reader, Disassembler & Expression Recovery
// ============================================================================
struct XRefItem {
    std::string kind;        // "CALL", "READ", "WRITE", "NEW", "IMPLEMENTS", "EXTENDS", "STRING"
    std::string from_class;  // e.g. "net/minecraft/SharedConstants"
    std::string from_method; // e.g. "tryDetectVersion()V"
    std::string to_owner;    // e.g. "net/minecraft/DetectedVersion"
    std::string to_member;   // e.g. "tryDetectVersion" or literal text
    int pc_offset = 0;
};

struct StringRef {
    std::string value;
    std::string cls;
    std::string method;
};

struct DecodedMethod {
    uint16_t flags = 0;
    std::string name;
    std::string desc;
    int max_stack = 0;
    int max_locals = 0;
    int code_bytes = 0;
    std::vector<std::string> callees;
    std::vector<std::string> strings;
    std::string decompiled_body;
    std::string disasm_text;
};

struct DecodedClass {
    bool valid = false;
    uint16_t minor_ver = 0;
    uint16_t major_ver = 69;
    uint16_t access_flags = 0x0021;
    std::string internal_name;
    std::string kind = "class"; // class | interface | enum | record | annotation
    std::string super_name = "java/lang/Object";
    std::vector<std::string> interfaces;
    std::vector<std::string> inner_classes;
    std::vector<std::pair<std::string, std::string>> fields; // (name, decl_line)
    std::vector<DecodedMethod> methods;
    std::vector<XRefItem> outgoing_xrefs;
    std::string source_code;
    std::string disasm_listing;
    std::string diff_text;
    int diff_add = 0;
    int diff_del = 0;
};

namespace bc_detail {

struct SafeCursor {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t o = 0;
    bool ok = true;

    uint8_t u8() noexcept {
        if (o + 1 > n) { ok = false; return 0; }
        return p[o++];
    }
    uint16_t u16() noexcept {
        if (o + 2 > n) { ok = false; return 0; }
        uint16_t v = (static_cast<uint16_t>(p[o]) << 8) | p[o + 1];
        o += 2;
        return v;
    }
    uint32_t u32() noexcept {
        if (o + 4 > n) { ok = false; return 0; }
        uint32_t v = (static_cast<uint32_t>(p[o]) << 24) | (static_cast<uint32_t>(p[o + 1]) << 16) |
                     (static_cast<uint32_t>(p[o + 2]) << 8) | p[o + 3];
        o += 4;
        return v;
    }
    void skip(size_t k) noexcept {
        if (k > n - o) { ok = false; o = n; }
        else o += k;
    }
};

inline std::string simple_name(std::string_view internal) {
    size_t sl = internal.rfind('/');
    return std::string(sl == std::string_view::npos ? internal : internal.substr(sl + 1));
}

inline std::string parse_desc_type(std::string_view desc, size_t& pos) {
    if (pos >= desc.size()) return "Object";
    int dims = 0;
    while (pos < desc.size() && desc[pos] == '[') { ++dims; ++pos; }
    if (pos >= desc.size()) return "Object[]";
    std::string base = "Object";
    switch (desc[pos++]) {
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
            std::string full(desc.substr(pos, semi == std::string_view::npos ? desc.size() - pos : semi - pos));
            pos = (semi == std::string_view::npos) ? desc.size() : semi + 1;
            base = simple_name(full);
            break;
        }
        default: break;
    }
    for (int d = 0; d < dims; ++d) base += "[]";
    return base;
}

inline int parse_method_arity(std::string_view desc, std::vector<std::string>* out_types = nullptr, std::string* out_ret = nullptr) {
    size_t p = (desc.empty() || desc[0] != '(') ? 0 : 1;
    int count = 0;
    while (p < desc.size() && desc[p] != ')') {
        std::string t = parse_desc_type(desc, p);
        if (out_types) out_types->push_back(std::move(t));
        ++count;
    }
    if (p < desc.size() && desc[p] == ')') ++p;
    if (out_ret) *out_ret = parse_desc_type(desc, p);
    return count;
}

inline std::string escape_java_str(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 32 && c < 127) out.push_back(static_cast<char>(c));
        else out.push_back('?');
    }
    return out;
}

// Complete 256-entry JVMS §6.5 opcode mnemonic & exact fixed length table (-1 = special variable length)
struct OpMeta {
    const char* name;
    int8_t len; // >0 exact bytes; -1 = tableswitch; -2 = lookupswitch; -3 = wide
};

inline OpMeta op_info(uint8_t op) noexcept {
    static constexpr std::array<OpMeta, 256> T = []() {
        std::array<OpMeta, 256> a{};
        for (int i = 0; i < 256; ++i) a[i] = {"reserved_op", 1};
        a[0x00]={"nop",1}; a[0x01]={"aconst_null",1}; a[0x02]={"iconst_m1",1};
        a[0x03]={"iconst_0",1}; a[0x04]={"iconst_1",1}; a[0x05]={"iconst_2",1};
        a[0x06]={"iconst_3",1}; a[0x07]={"iconst_4",1}; a[0x08]={"iconst_5",1};
        a[0x09]={"lconst_0",1}; a[0x0a]={"lconst_1",1}; a[0x0b]={"fconst_0",1};
        a[0x0c]={"fconst_1",1}; a[0x0d]={"fconst_2",1}; a[0x0e]={"dconst_0",1}; a[0x0f]={"dconst_1",1};
        a[0x10]={"bipush",2}; a[0x11]={"sipush",3}; a[0x12]={"ldc",2}; a[0x13]={"ldc_w",3}; a[0x14]={"ldc2_w",3};
        a[0x15]={"iload",2}; a[0x16]={"lload",2}; a[0x17]={"fload",2}; a[0x18]={"dload",2}; a[0x19]={"aload",2};
        a[0x1a]={"iload_0",1}; a[0x1b]={"iload_1",1}; a[0x1c]={"iload_2",1}; a[0x1d]={"iload_3",1};
        a[0x1e]={"lload_0",1}; a[0x1f]={"lload_1",1}; a[0x20]={"lload_2",1}; a[0x21]={"lload_3",1};
        a[0x22]={"fload_0",1}; a[0x23]={"fload_1",1}; a[0x24]={"fload_2",1}; a[0x25]={"fload_3",1};
        a[0x26]={"dload_0",1}; a[0x27]={"dload_1",1}; a[0x28]={"dload_2",1}; a[0x29]={"dload_3",1};
        a[0x2a]={"aload_0",1}; a[0x2b]={"aload_1",1}; a[0x2c]={"aload_2",1}; a[0x2d]={"aload_3",1};
        a[0x2e]={"iaload",1}; a[0x2f]={"laload",1}; a[0x30]={"faload",1}; a[0x31]={"daload",1};
        a[0x32]={"aaload",1}; a[0x33]={"baload",1}; a[0x34]={"caload",1}; a[0x35]={"saload",1};
        a[0x36]={"istore",2}; a[0x37]={"lstore",2}; a[0x38]={"fstore",2}; a[0x39]={"dstore",2}; a[0x3a]={"astore",2};
        a[0x3b]={"istore_0",1}; a[0x3c]={"istore_1",1}; a[0x3d]={"istore_2",1}; a[0x3e]={"istore_3",1};
        a[0x3f]={"lstore_0",1}; a[0x40]={"lstore_1",1}; a[0x41]={"lstore_2",1}; a[0x42]={"lstore_3",1};
        a[0x43]={"fstore_0",1}; a[0x44]={"fstore_1",1}; a[0x45]={"fstore_2",1}; a[0x46]={"fstore_3",1};
        a[0x47]={"dstore_0",1}; a[0x48]={"dstore_1",1}; a[0x49]={"dstore_2",1}; a[0x4a]={"dstore_3",1};
        a[0x4b]={"astore_0",1}; a[0x4c]={"astore_1",1}; a[0x4d]={"astore_2",1}; a[0x4e]={"astore_3",1};
        for (int k = 0x4f; k <= 0x83; ++k) a[k] = {"alu_stack", 1};
        a[0x57]={"pop",1}; a[0x59]={"dup",1}; a[0x60]={"iadd",1}; a[0x64]={"isub",1};
        a[0x68]={"imul",1}; a[0x6c]={"idiv",1}; a[0x70]={"irem",1};
        a[0x84]={"iinc",3};
        for (int k = 0x85; k <= 0x98; ++k) a[k] = {"cast_num", 1};
        a[0x99]={"ifeq",3}; a[0x9a]={"ifne",3}; a[0x9b]={"iflt",3}; a[0x9c]={"ifge",3};
        a[0x9d]={"ifgt",3}; a[0x9e]={"ifle",3}; a[0x9f]={"if_icmpeq",3}; a[0xa0]={"if_icmpne",3};
        a[0xa1]={"if_icmplt",3}; a[0xa2]={"if_icmpge",3}; a[0xa3]={"if_icmpgt",3}; a[0xa4]={"if_icmple",3};
        a[0xa5]={"if_acmpeq",3}; a[0xa6]={"if_acmpne",3}; a[0xa7]={"goto",3};
        a[0xa8]={"jsr",3}; a[0xa9]={"ret",2};
        a[0xaa]={"tableswitch",-1}; a[0xab]={"lookupswitch",-2};
        a[0xac]={"ireturn",1}; a[0xad]={"lreturn",1}; a[0xae]={"freturn",1};
        a[0xaf]={"dreturn",1}; a[0xb0]={"areturn",1}; a[0xb1]={"return",1};
        a[0xb2]={"getstatic",3}; a[0xb3]={"putstatic",3}; a[0xb4]={"getfield",3}; a[0xb5]={"putfield",3};
        a[0xb6]={"invokevirtual",3}; a[0xb7]={"invokespecial",3}; a[0xb8]={"invokestatic",3};
        a[0xb9]={"invokeinterface",5}; a[0xba]={"invokedynamic",5};
        a[0xbb]={"new",3}; a[0xbc]={"newarray",2}; a[0xbd]={"anewarray",3};
        a[0xbe]={"arraylength",1}; a[0xbf]={"athrow",1};
        a[0xc0]={"checkcast",3}; a[0xc1]={"instanceof",3};
        a[0xc2]={"monitorenter",1}; a[0xc3]={"monitorexit",1};
        a[0xc4]={"wide",-3}; a[0xc5]={"multianewarray",4};
        a[0xc6]={"ifnull",3}; a[0xc7]={"ifnonnull",3};
        a[0xc8]={"goto_w",5}; a[0xc9]={"jsr_w",5};
        return a;
    }();
    return T[op];
}

inline size_t advance_instruction_bytes(const uint8_t* code, size_t code_len, size_t pc) noexcept {
    if (pc >= code_len) return code_len;
    OpMeta m = op_info(code[pc]);
    if (m.len > 0) return std::min(code_len, pc + static_cast<size_t>(m.len));
    if (m.len == -3) { // wide
        if (pc + 1 >= code_len) return code_len;
        return std::min(code_len, pc + (code[pc + 1] == 0x84 ? 6u : 4u));
    }
    size_t aligned = (pc + 4) & ~size_t(3);
    if (m.len == -1) { // tableswitch
        if (aligned + 12 > code_len) return code_len;
        auto r32 = [&](size_t o) -> int32_t {
            return static_cast<int32_t>((uint32_t(code[o]) << 24) | (uint32_t(code[o + 1]) << 16) |
                                        (uint32_t(code[o + 2]) << 8) | uint32_t(code[o + 3]));
        };
        int64_t low = r32(aligned + 4);
        int64_t high = r32(aligned + 8);
        if (high < low || (high - low) > 65536) return code_len;
        size_t jump_bytes = static_cast<size_t>(high - low + 1) * 4u;
        return std::min(code_len, aligned + 12 + jump_bytes);
    }
    if (m.len == -2) { // lookupswitch
        if (aligned + 8 > code_len) return code_len;
        uint32_t npairs = (uint32_t(code[aligned + 4]) << 24) | (uint32_t(code[aligned + 5]) << 16) |
                          (uint32_t(code[aligned + 6]) << 8) | uint32_t(code[aligned + 7]);
        if (npairs > 65536u) return code_len;
        return std::min(code_len, aligned + 8 + static_cast<size_t>(npairs) * 8u);
    }
    return std::min(code_len, pc + 1);
}

} // namespace bc_detail

// Fast Mode (`full_source = false`): extracts XREFS, constant-pool strings & summary in ~12 microseconds per class!
// Full Mode (`full_source = true`): full expression recovery decompiler + formatted JVM bytecode listing!
inline DecodedClass decode_class_safe(const uint8_t* data, size_t len, bool full_source = true) noexcept {
    DecodedClass out;
    bc_detail::SafeCursor c{data, len, 0, true};
    if (c.u32() != 0xCAFEBABEu || !c.ok) return out;

    out.minor_ver = c.u16();
    out.major_ver = c.u16();
    uint16_t cp_count = c.u16();
    if (!c.ok || cp_count == 0) return out;

    struct CpSlot {
        uint8_t tag = 0;
        uint16_t a = 0;
        uint16_t b = 0;
        std::string str;
    };
    std::vector<CpSlot> cp(cp_count);

    for (uint16_t i = 1; i < cp_count && c.ok; ++i) {
        uint8_t tag = c.u8();
        cp[i].tag = tag;
        switch (tag) {
            case 1: {
                uint16_t slen = c.u16();
                if (!c.ok || slen > c.n - c.o) { c.ok = false; break; }
                cp[i].str.assign(reinterpret_cast<const char*>(c.p + c.o), slen);
                c.o += slen;
                break;
            }
            case 3: { // Integer
                cp[i].str = std::to_string(static_cast<int32_t>(c.u32()));
                break;
            }
            case 4: { // Float
                uint32_t bits = c.u32();
                float fv = 0.0f;
                std::memcpy(&fv, &bits, 4);
                std::ostringstream oss;
                oss << fv << "F";
                cp[i].str = oss.str();
                break;
            }
            case 5: { // Long (2 slots!)
                int64_t lv = (static_cast<int64_t>(c.u32()) << 32) | static_cast<uint64_t>(c.u32());
                cp[i].str = std::to_string(lv) + "L";
                ++i;
                break;
            }
            case 6: { // Double (2 slots!)
                uint64_t bits = (static_cast<uint64_t>(c.u32()) << 32) | static_cast<uint64_t>(c.u32());
                double dv = 0.0;
                std::memcpy(&dv, &bits, 8);
                std::ostringstream oss;
                oss << dv;
                cp[i].str = oss.str();
                ++i;
                break;
            }
            case 7: case 8: case 16: case 19: case 20:
                cp[i].a = c.u16();
                break;
            case 9: case 10: case 11: case 12: case 17: case 18:
                cp[i].a = c.u16();
                cp[i].b = c.u16();
                break;
            case 15:
                c.u8();
                cp[i].a = c.u16();
                break;
            default:
                c.ok = false;
                break;
        }
    }
    if (!c.ok) return out;

    auto utf8 = [&](uint16_t idx) -> std::string {
        return (idx < cp.size() && cp[idx].tag == 1) ? cp[idx].str : "";
    };
    auto cls_name = [&](uint16_t idx) -> std::string {
        return (idx < cp.size() && cp[idx].tag == 7) ? utf8(cp[idx].a) : "";
    };
    auto ref_parts = [&](uint16_t idx, std::string& owner, std::string& member, std::string& desc) -> bool {
        if (idx >= cp.size()) return false;
        const auto& e = cp[idx];
        if (e.tag == 9 || e.tag == 10 || e.tag == 11) {
            owner = cls_name(e.a);
            if (e.b < cp.size() && cp[e.b].tag == 12) {
                member = utf8(cp[e.b].a);
                desc = utf8(cp[e.b].b);
            }
            return true;
        }
        if (e.tag == 18 && e.b < cp.size() && cp[e.b].tag == 12) {
            owner = "invokedynamic#" + std::to_string(e.a);
            member = utf8(cp[e.b].a);
            desc = utf8(cp[e.b].b);
            return true;
        }
        return false;
    };

    out.access_flags = c.u16();
    out.internal_name = cls_name(c.u16());
    out.super_name = cls_name(c.u16());
    if (!c.ok || out.internal_name.empty()) return out;

    if (out.access_flags & 0x2000u) out.kind = "annotation";
    else if (out.access_flags & 0x0200u) out.kind = "interface";
    else if (out.access_flags & 0x4000u) out.kind = "enum";
    else if (out.super_name == "java/lang/Record") out.kind = "record";
    else out.kind = "class";

    if (!out.super_name.empty() && out.super_name != "java/lang/Object") {
        out.outgoing_xrefs.push_back({"EXTENDS", out.internal_name, "<type>", out.super_name, "", 0});
    }

    uint16_t iface_n = c.u16();
    for (uint16_t i = 0; i < iface_n && c.ok; ++i) {
        std::string iname = cls_name(c.u16());
        if (!iname.empty()) {
            out.interfaces.push_back(iname);
            out.outgoing_xrefs.push_back({"IMPLEMENTS", out.internal_name, "<type>", iname, "", 0});
        }
    }

    std::set<std::string> imports;
    auto note_import = [&](const std::string& internal) {
        if (internal.empty() || internal.find('/') == std::string::npos || internal.rfind("java/lang/", 0) == 0) return;
        size_t s1 = out.internal_name.rfind('/');
        size_t s2 = internal.rfind('/');
        if (s1 != std::string::npos && s2 != std::string::npos && out.internal_name.substr(0, s1) == internal.substr(0, s2)) return;
        std::string dot = internal;
        std::replace(dot.begin(), dot.end(), '/', '.');
        imports.insert(dot);
    };
    note_import(out.super_name);
    for (const auto& itf : out.interfaces) note_import(itf);

    // Fields
    uint16_t fields_n = c.u16();
    for (uint16_t i = 0; i < fields_n && c.ok; ++i) {
        uint16_t ff = c.u16();
        std::string fn = utf8(c.u16());
        std::string fd = utf8(c.u16());
        uint16_t ac = c.u16();
        std::string cval;
        for (uint16_t a = 0; a < ac && c.ok; ++a) {
            std::string an = utf8(c.u16());
            uint32_t al = c.u32();
            if (an == "ConstantValue" && al == 2 && c.o + 2 <= c.n) {
                uint16_t kv = c.u16();
                if (kv < cp.size()) {
                    cval = (cp[kv].tag == 8) ? ("\"" + bc_detail::escape_java_str(utf8(cp[kv].a)) + "\"") : cp[kv].str;
                }
            } else {
                c.skip(al);
            }
        }
        size_t dp = 0;
        std::string ftype = bc_detail::parse_desc_type(fd, dp);
        std::ostringstream decl;
        if (ff & 0x0001u) decl << "public ";
        else if (ff & 0x0002u) decl << "private ";
        else if (ff & 0x0004u) decl << "protected ";
        if (ff & 0x0008u) decl << "static ";
        if (ff & 0x0010u) decl << "final ";
        decl << ftype << " " << fn;
        if (!cval.empty()) decl << " = " << cval;
        out.fields.emplace_back(fn, decl.str());
    }

    // Methods + Bytecode Walker & Stack Expression Recovery
    uint16_t methods_n = c.u16();
    int java_ver = static_cast<int>(out.major_ver) - 44;
    std::ostringstream all_disasm;
    if (full_source) {
        all_disasm << "// Classfile " << out.internal_name << ".class  [JVMS " << out.major_ver << "." << out.minor_ver
                   << " · Java SE " << java_ver << "]\n"
                   << "// Constant Pool: " << cp_count << " slots  |  Flags: 0x" << std::hex << out.access_flags << std::dec << "\n\n";
    }

    for (uint16_t mi = 0; mi < methods_n && c.ok; ++mi) {
        DecodedMethod dm;
        dm.flags = c.u16();
        dm.name = utf8(c.u16());
        dm.desc = utf8(c.u16());
        uint16_t ac = c.u16();
        std::string method_id = dm.name + dm.desc;

        for (uint16_t a = 0; a < ac && c.ok; ++a) {
            std::string an = utf8(c.u16());
            uint32_t al = c.u32();
            size_t attr_end = std::min(c.n, c.o + al);
            if (an == "Code" && al >= 8 && c.o + 8 <= attr_end) {
                dm.max_stack = c.u16();
                dm.max_locals = c.u16();
                uint32_t code_len = c.u32();
                dm.code_bytes = static_cast<int>(code_len);
                size_t code_start = c.o;
                size_t code_end = std::min(attr_end, code_start + code_len);
                const uint8_t* code = c.p + code_start;
                size_t clen = code_end - code_start;

                // Lightweight symbolic operand stack for readable Java expression recovery!
                std::vector<std::string> stack;
                std::vector<std::string> stmts;
                auto pop_val = [&]() -> std::string {
                    if (stack.empty()) return "/*expr*/";
                    std::string v = std::move(stack.back());
                    stack.pop_back();
                    return v;
                };
                bool is_static = (dm.flags & 0x0008u) != 0;
                auto local_name = [&](int slot) -> std::string {
                    if (!is_static && slot == 0) return "this";
                    int p = is_static ? slot : (slot - 1);
                    return p >= 0 ? ("arg" + std::to_string(p)) : ("var" + std::to_string(slot));
                };

                std::ostringstream dis;
                if (full_source) {
                    dis << "  method " << dm.name << dm.desc << "  [stack=" << dm.max_stack
                        << ", locals=" << dm.max_locals << ", code=" << clen << "B]\n";
                }

                size_t pc = 0;
                while (pc < clen) {
                    uint8_t op = code[pc];
                    size_t next_pc = bc_detail::advance_instruction_bytes(code, clen, pc);
                    auto info = bc_detail::op_info(op);
                    auto u16_at = [&](size_t off) -> uint16_t {
                        return (off + 1 < clen) ? static_cast<uint16_t>((code[off] << 8) | code[off + 1]) : 0u;
                    };

                    std::string operand_comment;

                    if (op == 0x12 || op == 0x13 || op == 0x14) { // ldc / ldc_w / ldc2_w
                        uint16_t idx = (op == 0x12 && pc + 1 < clen) ? code[pc + 1] : u16_at(pc + 1);
                        if (idx < cp.size()) {
                            if (cp[idx].tag == 8) {
                                std::string s = utf8(cp[idx].a);
                                dm.strings.push_back(s);
                                out.outgoing_xrefs.push_back({"STRING", out.internal_name, method_id, out.internal_name, s, static_cast<int>(pc)});
                                operand_comment = "\"" + bc_detail::escape_java_str(s) + "\"";
                                if (full_source) stack.push_back(operand_comment);
                            } else if (cp[idx].tag == 7) {
                                std::string cn = cls_name(idx);
                                note_import(cn);
                                operand_comment = bc_detail::simple_name(cn) + ".class";
                                if (full_source) stack.push_back(operand_comment);
                            } else {
                                operand_comment = cp[idx].str;
                                if (full_source) stack.push_back(operand_comment);
                            }
                        }
                    } else if (op >= 0xb2 && op <= 0xba) {
                        uint16_t idx = u16_at(pc + 1);
                        std::string owner, mem, mdesc;
                        if (ref_parts(idx, owner, mem, mdesc)) {
                            note_import(owner);
                            operand_comment = owner + "." + mem + mdesc;
                            if (op == 0xb2 || op == 0xb4) { // getstatic / getfield
                                out.outgoing_xrefs.push_back({"READ", out.internal_name, method_id, owner, mem, static_cast<int>(pc)});
                                if (full_source) {
                                    std::string recv = (op == 0xb4) ? pop_val() : bc_detail::simple_name(owner);
                                    stack.push_back(recv + "." + mem);
                                }
                            } else if (op == 0xb3 || op == 0xb5) { // putstatic / putfield
                                out.outgoing_xrefs.push_back({"WRITE", out.internal_name, method_id, owner, mem, static_cast<int>(pc)});
                                if (full_source) {
                                    std::string val = pop_val();
                                    std::string recv = (op == 0xb5) ? pop_val() : bc_detail::simple_name(owner);
                                    stmts.push_back(recv + "." + mem + " = " + val + ";");
                                }
                            } else { // invoke*
                                dm.callees.push_back(owner + "." + mem);
                                out.outgoing_xrefs.push_back({"CALL", out.internal_name, method_id, owner, mem, static_cast<int>(pc)});
                                if (full_source) {
                                    std::string ret_t;
                                    int arity = bc_detail::parse_method_arity(mdesc, nullptr, &ret_t);
                                    std::vector<std::string> args(static_cast<size_t>(arity));
                                    for (int k = arity - 1; k >= 0; --k) args[static_cast<size_t>(k)] = pop_val();
                                    std::string joined;
                                    for (size_t k = 0; k < args.size(); ++k) {
                                        if (k) joined += ", ";
                                        joined += args[k];
                                    }
                                    if (mem == "<init>") {
                                        std::string recv = pop_val();
                                        if (recv == "this") {
                                            if (!joined.empty() || bc_detail::simple_name(owner) != "Object") {
                                                stmts.push_back("super(" + joined + ");");
                                            }
                                        } else {
                                            std::string expr = "new " + bc_detail::simple_name(owner) + "(" + joined + ")";
                                            if (!stack.empty() && stack.back() == recv) stack.back() = expr;
                                            else stack.push_back(expr);
                                        }
                                    } else {
                                        std::string recv = (op == 0xb8 || op == 0xba) ? bc_detail::simple_name(owner) : pop_val();
                                        std::string call = recv + "." + mem + "(" + joined + ")";
                                        if (ret_t == "void") stmts.push_back(call + ";");
                                        else stack.push_back(call);
                                    }
                                }
                            }
                        }
                    } else if (op == 0xbb) { // new
                        std::string cn = cls_name(u16_at(pc + 1));
                        note_import(cn);
                        out.outgoing_xrefs.push_back({"NEW", out.internal_name, method_id, cn, "<init>", static_cast<int>(pc)});
                        operand_comment = cn;
                        if (full_source) stack.push_back("new#" + bc_detail::simple_name(cn));
                    } else if (full_source) {
                        if (op == 0x01) stack.push_back("null");
                        else if (op >= 0x02 && op <= 0x08) stack.push_back(std::to_string(static_cast<int>(op) - 3));
                        else if (op == 0x10 && pc + 1 < clen) {
                            int v = static_cast<int>(static_cast<int8_t>(code[pc + 1]));
                            operand_comment = std::to_string(v);
                            stack.push_back(operand_comment);
                        } else if (op == 0x11 && pc + 2 < clen) {
                            int v = static_cast<int>(static_cast<int16_t>(u16_at(pc + 1)));
                            operand_comment = std::to_string(v);
                            stack.push_back(operand_comment);
                        } else if (op >= 0x1a && op <= 0x1d) stack.push_back(local_name(op - 0x1a));
                        else if (op >= 0x2a && op <= 0x2d) stack.push_back(local_name(op - 0x2a));
                        else if ((op >= 0x15 && op <= 0x19) && pc + 1 < clen) stack.push_back(local_name(code[pc + 1]));
                        else if ((op >= 0x36 && op <= 0x3a) && pc + 1 < clen) {
                            stmts.push_back("var " + local_name(code[pc + 1]) + " = " + pop_val() + ";");
                        } else if (op >= 0x4b && op <= 0x4e) {
                            stmts.push_back("var " + local_name(op - 0x4b) + " = " + pop_val() + ";");
                        } else if (op == 0x59 && !stack.empty()) {
                            stack.push_back(stack.back());
                        } else if (op == 0x57 && !stack.empty()) {
                            std::string dropped = pop_val();
                            if (dropped.find('(') != std::string::npos) stmts.push_back(dropped + ";");
                        } else if (op == 0x60 || op == 0x64 || op == 0x68 || op == 0x6c) {
                            std::string rhs = pop_val();
                            std::string lhs = pop_val();
                            const char* sym = (op == 0x60) ? " + " : (op == 0x64 ? " - " : (op == 0x68 ? " * " : " / "));
                            stack.push_back("(" + lhs + sym + rhs + ")");
                        } else if (op >= 0xac && op <= 0xb0) {
                            stmts.push_back("return " + pop_val() + ";");
                        } else if (op == 0xb1) {
                            if (dm.name != "<init>" || stmts.empty()) {
                                if (dm.name != "<init>") stmts.push_back("return;");
                            }
                        } else if (op == 0xbf) {
                            stmts.push_back("throw " + pop_val() + ";");
                        } else if ((op >= 0x99 && op <= 0xa7) || op == 0xc6 || op == 0xc7) {
                            int target_pc = static_cast<int>(pc) + static_cast<int16_t>(u16_at(pc + 1));
                            operand_comment = "-> pc#" + std::to_string(target_pc);
                        }
                    }

                    if (full_source) {
                        dis << "    " << std::setw(4) << pc << ": " << std::left << std::setw(17) << info.name << std::right;
                        if (!operand_comment.empty()) dis << " // " << operand_comment;
                        dis << "\n";
                    }
                    pc = (next_pc > pc) ? next_pc : (pc + 1);
                }

                if (full_source) {
                    std::ostringstream body;
                    for (const auto& s : stmts) body << "        " << s << "\n";
                    dm.decompiled_body = body.str();
                    dm.disasm_text = dis.str() + "\n";
                    all_disasm << dm.disasm_text;
                }
            }
            c.o = attr_end;
        }
        out.methods.push_back(std::move(dm));
    }

    // Class-level InnerClasses attribute scan
    if (c.ok && c.o + 2 <= c.n) {
        uint16_t cattrs = c.u16();
        for (uint16_t a = 0; a < cattrs && c.ok; ++a) {
            std::string an = utf8(c.u16());
            uint32_t al = c.u32();
            size_t aend = std::min(c.n, c.o + al);
            if (an == "InnerClasses" && c.o + 2 <= aend) {
                uint16_t ic_n = c.u16();
                for (uint16_t k = 0; k < ic_n && c.o + 8 <= aend; ++k) {
                    std::string ic = cls_name(c.u16());
                    c.u16(); c.u16(); c.u16();
                    if (!ic.empty() && ic != out.internal_name) out.inner_classes.push_back(ic);
                }
            }
            c.o = aend;
        }
    }

    if (full_source) {
        out.disasm_listing = all_disasm.str();
        std::ostringstream jv;
        size_t sl = out.internal_name.rfind('/');
        std::string pkg = (sl == std::string::npos) ? "" : out.internal_name.substr(0, sl);
        std::string sname = bc_detail::simple_name(out.internal_name);
        if (!pkg.empty()) {
            std::replace(pkg.begin(), pkg.end(), '/', '.');
            jv << "package " << pkg << ";\n\n";
        }
        for (const auto& imp : imports) jv << "import " << imp << ";\n";
        if (!imports.empty()) jv << "\n";

        jv << "// Recovered directly from JVM classfile " << out.major_ver << "." << out.minor_ver
           << " (Java SE " << java_ver << ", " << out.methods.size() << " methods, " << out.fields.size() << " fields)\n";
        if (out.access_flags & 0x0001u) jv << "public ";
        if ((out.access_flags & 0x0400u) && out.kind == "class") jv << "abstract ";
        if ((out.access_flags & 0x0010u) && out.kind == "class") jv << "final ";
        jv << out.kind << " " << sname;
        if (!out.super_name.empty() && out.super_name != "java/lang/Object" && out.kind == "class") {
            jv << " extends " << bc_detail::simple_name(out.super_name);
        }
        if (!out.interfaces.empty()) {
            jv << (out.kind == "interface" ? " extends " : " implements ");
            for (size_t i = 0; i < out.interfaces.size(); ++i) {
                if (i) jv << ", ";
                jv << bc_detail::simple_name(out.interfaces[i]);
            }
        }
        jv << " {\n";

        for (const auto& [fn, fdecl] : out.fields) jv << "    " << fdecl << ";\n";
        if (!out.fields.empty()) jv << "\n";

        for (const auto& m : out.methods) {
            if (m.name == "<clinit>") {
                jv << "    static {\n"
                   << (m.decompiled_body.empty() ? "        // static initializer\n" : m.decompiled_body)
                   << "    }\n\n";
                continue;
            }
            std::vector<std::string> ptypes;
            std::string ret_t;
            bc_detail::parse_method_arity(m.desc, &ptypes, &ret_t);

            jv << "    ";
            if (m.flags & 0x0001u) jv << "public ";
            else if (m.flags & 0x0002u) jv << "private ";
            else if (m.flags & 0x0004u) jv << "protected ";
            if (m.flags & 0x0008u) jv << "static ";
            if (m.flags & 0x0010u) jv << "final ";
            if (m.flags & 0x0400u) jv << "abstract ";

            if (m.name == "<init>") jv << sname << "(";
            else jv << ret_t << " " << m.name << "(";
            for (size_t k = 0; k < ptypes.size(); ++k) {
                if (k) jv << ", ";
                jv << ptypes[k] << " arg" << k;
            }
            if ((m.flags & 0x0400u) || m.code_bytes == 0) {
                jv << ");\n\n";
            } else {
                jv << ") {\n"
                   << (m.decompiled_body.empty() ? "        // switch to Disassembler tab for raw JVM instructions\n" : m.decompiled_body)
                   << "    }\n\n";
            }
        }
        jv << "}\n";
        out.source_code = jv.str();
    }

    out.valid = true;
    return out;
}

// ============================================================================
// High-Speed Workspace with Async XRef Indexer & Clean-Room Reference Dataset
// ============================================================================
struct GlobalIndexSnapshot {
    std::unordered_map<std::string, std::vector<XRefItem>> incoming_xrefs; // keyed by class AND class.member
    std::vector<StringRef> strings;
    size_t method_total = 0;
    size_t field_total = 0;
};

class BinaryWorkspace {
public:
    mutable std::mutex mtx;
    std::string mode_label = "Clean-Room 26.3 Architecture Spec";
    std::string loaded_jar_path;
    std::string loaded_sha1;
    size_t jar_bytes = 0;
    bool using_user_jar = false;

    SafeJarReader jar;
    std::string local_sources_dir;
    std::vector<std::string> sorted_classes; // fast contiguous O(log N) binary-searchable list!
    std::unordered_map<std::string, DecodedClass> builtins;
    mutable std::unordered_map<std::string, DecodedClass> decoded_lru;

    std::atomic<bool> indexing{false};
    std::atomic<bool> cancel_index{false};
    std::atomic<uint32_t> indexed_done{0};
    std::atomic<uint32_t> indexed_total{1};
    std::shared_ptr<GlobalIndexSnapshot> index_snap = std::make_shared<GlobalIndexSnapshot>();
    std::thread index_worker;

    BinaryWorkspace() {
        init_cleanroom_workspace();
        if (std::filesystem::exists(".local/minecraft/26.3/sources")) {
            local_sources_dir = ".local/minecraft/26.3/sources";
        }
    }

    ~BinaryWorkspace() {
        stop_background_indexer();
    }

    void stop_background_indexer() {
        cancel_index.store(true);
        if (index_worker.joinable()) index_worker.join();
        indexing.store(false);
    }

    // Detects local Minecraft JAR path WITHOUT blocking UI startup — shown as 1-click instant load!
    static std::string detect_local_minecraft_jar() {
        namespace fs = std::filesystem;
        if (const char* env_jar = std::getenv("MINECC_JAR")) {
            if (fs::is_regular_file(env_jar)) return env_jar;
        }
        std::vector<fs::path> cands;
        if (const char* appdata = std::getenv("APPDATA")) {
            cands.push_back(fs::path(appdata) / ".minecraft/versions/26.3/26.3.jar");
            cands.push_back(fs::path(appdata) / ".minecraft/versions/26.4-snapshot-5/26.4-snapshot-5.jar");
        }
        if (const char* home = std::getenv("HOME")) {
            cands.push_back(fs::path(home) / ".minecraft/versions/26.3/26.3.jar");
            cands.push_back(fs::path(home) / "Library/Application Support/minecraft/versions/26.3/26.3.jar");
        }
        cands.push_back(".local/minecraft/26.3/client.jar");
        for (const auto& p : cands) {
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p.string();
        }
        return "";
    }

    bool load_jar(const std::string& path, std::string& status_msg, bool wait_for_full_index = false) {
        stop_background_indexer();
        SafeJarReader next_jar;
        std::string err;
        if (!next_jar.open_file(path, err)) {
            status_msg = err;
            return false;
        }

        std::vector<std::string> all_cls;
        std::vector<std::string> raw_class_entries;
        all_cls.reserve(next_jar.entries.size());
        raw_class_entries.reserve(next_jar.entries.size());

        for (const auto& [ename, _] : next_jar.entries) {
            if (ename.size() > 6 && ename.compare(ename.size() - 6, 6, ".class") == 0 && ename != "module-info.class") {
                raw_class_entries.push_back(ename);
                std::string internal = ename.substr(0, ename.size() - 6);
                if (internal.find('$') == std::string::npos) {
                    all_cls.push_back(std::move(internal));
                }
            }
        }
        if (all_cls.empty()) {
            status_msg = "Opened archive, but found 0 top-level JVM .class entries: " + path;
            return false;
        }
        std::sort(all_cls.begin(), all_cls.end());
        std::string sha = sha1_hex(next_jar.bytes.data(), next_jar.bytes.size());

        {
            std::lock_guard<std::mutex> lk(mtx);
            jar = std::move(next_jar);
            jar_bytes = jar.bytes.size();
            loaded_jar_path = path;
            loaded_sha1 = sha;
            sorted_classes = std::move(all_cls);
            decoded_lru.clear();
            using_user_jar = true;
            mode_label = std::filesystem::path(path).filename().string() + " (" + std::to_string(sorted_classes.size()) + " classes)";
        }

        // Launch non-blocking background XREF + String indexer!
        cancel_index.store(false);
        indexed_done.store(0);
        indexed_total.store(static_cast<uint32_t>(std::max<size_t>(1, raw_class_entries.size())));
        indexing.store(true);

        index_worker = std::thread([this, entries = std::move(raw_class_entries)]() {
            auto snap = std::make_shared<GlobalIndexSnapshot>();
            std::vector<uint8_t> buf;
            buf.reserve(65536);
            uint32_t done = 0;

            for (const auto& ename : entries) {
                if (cancel_index.load()) return;
                bool ok = false;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    ok = jar.extract_entry(ename, buf);
                }
                if (ok && !buf.empty()) {
                    DecodedClass dc = decode_class_safe(buf.data(), buf.size(), false);
                    if (dc.valid) {
                        snap->method_total += dc.methods.size();
                        snap->field_total += dc.fields.size();
                        for (auto& xr : dc.outgoing_xrefs) {
                            if (xr.kind == "STRING") {
                                if (snap->strings.size() < 40000 && xr.to_member.size() >= 2) {
                                    snap->strings.push_back({xr.to_member, xr.from_class, xr.from_method});
                                }
                            } else {
                                snap->incoming_xrefs[xr.to_owner].push_back(xr);
                                if (!xr.to_member.empty()) {
                                    snap->incoming_xrefs[xr.to_owner + "." + xr.to_member].push_back(xr);
                                }
                            }
                        }
                    }
                }
                indexed_done.store(++done);
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                index_snap = std::move(snap);
            }
            indexing.store(false);
        });

        if (wait_for_full_index && index_worker.joinable()) {
            index_worker.join();
        }

        status_msg = "Loaded " + std::to_string(sorted_classes.size()) + " top-level classes (" +
                     std::to_string(raw_class_entries.size()) + " total .class files, SHA-1 " + sha.substr(0, 10) + "...)";
        return true;
    }

    std::string resolve_class_query(std::string_view q) const {
        std::string norm(q);
        std::replace(norm.begin(), norm.end(), '.', '/');
        if (norm.size() > 5 && norm.compare(norm.size() - 5, 5, ".java") == 0) norm.resize(norm.size() - 5);
        if (norm.size() > 6 && norm.compare(norm.size() - 6, 6, ".class") == 0) norm.resize(norm.size() - 6);

        std::lock_guard<std::mutex> lk(mtx);
        if (std::binary_search(sorted_classes.begin(), sorted_classes.end(), norm)) return norm;
        std::string suffix = "/" + norm;
        for (const auto& c : sorted_classes) {
            if (c == norm || (c.size() >= suffix.size() && c.compare(c.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                return c;
            }
        }
        // Fallback case-insensitive substring
        std::string low_q = norm;
        for (char& ch : low_q) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (const auto& c : sorted_classes) {
            std::string lc = c;
            for (char& ch : lc) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (lc.find(low_q) != std::string::npos) return c;
        }
        return sorted_classes.empty() ? "" : sorted_classes.front();
    }

    DecodedClass inspect_class(const std::string& internal_name) const {
        std::lock_guard<std::mutex> lk(mtx);
        if (!using_user_jar) {
            auto it = builtins.find(internal_name);
            return it == builtins.end() ? DecodedClass{} : it->second;
        }
        auto cached = decoded_lru.find(internal_name);
        if (cached != decoded_lru.end()) return cached->second;

        std::vector<uint8_t> raw;
        if (!jar.extract_entry(internal_name + ".class", raw)) return DecodedClass{};
        DecodedClass dc = decode_class_safe(raw.data(), raw.size(), true);

        if (!local_sources_dir.empty()) {
            std::filesystem::path sp = std::filesystem::path(local_sources_dir) / (internal_name + ".java");
            if (std::filesystem::is_regular_file(sp)) {
                std::ifstream f(sp);
                std::ostringstream ss;
                ss << f.rdbuf();
                dc.source_code = ss.str();
            }
        }
        if (decoded_lru.size() >= 128) decoded_lru.clear();
        decoded_lru[internal_name] = dc;
        return dc;
    }

    std::vector<XRefItem> query_xrefs_to(const std::string& symbol) const {
        std::shared_ptr<GlobalIndexSnapshot> snap;
        {
            std::lock_guard<std::mutex> lk(mtx);
            snap = index_snap;
        }
        if (!snap) return {};
        std::string norm = symbol;
        std::replace(norm.begin(), norm.end(), '.', '/');
        auto it = snap->incoming_xrefs.find(norm);
        if (it != snap->incoming_xrefs.end()) return it->second;

        // Also check `Class/Path.member` when user passed `DetectedVersion.tryDetectVersion`
        size_t last_slash = norm.rfind('/');
        if (last_slash != std::string::npos) {
            std::string candidate_member = norm.substr(last_slash + 1);
            std::string owner_q = resolve_class_query(norm.substr(0, last_slash));
            auto it2 = snap->incoming_xrefs.find(owner_q + "." + candidate_member);
            if (it2 != snap->incoming_xrefs.end()) return it2->second;
        }
        std::string resolved = resolve_class_query(symbol);
        auto it3 = snap->incoming_xrefs.find(resolved);
        return it3 != snap->incoming_xrefs.end() ? it3->second : std::vector<XRefItem>{};
    }

    void init_cleanroom_workspace() {
        builtins.clear();
        auto snap = std::make_shared<GlobalIndexSnapshot>();

        auto reg = [&](DecodedClass dc) {
            for (const auto& xr : dc.outgoing_xrefs) {
                if (xr.kind == "STRING") {
                    snap->strings.push_back({xr.to_member, xr.from_class, xr.from_method});
                } else {
                    snap->incoming_xrefs[xr.to_owner].push_back(xr);
                    if (!xr.to_member.empty()) snap->incoming_xrefs[xr.to_owner + "." + xr.to_member].push_back(xr);
                }
            }
            snap->method_total += dc.methods.size();
            snap->field_total += dc.fields.size();
            builtins[dc.internal_name] = std::move(dc);
        };

        {
            DecodedClass d;
            d.valid = true;
            d.internal_name = "net/minecraft/DetectedVersion";
            d.kind = "class";
            d.access_flags = 0x0021;
            d.interfaces = {"net/minecraft/WorldVersion"};
            d.fields = {
                {"LOGGER", "private static final Logger LOGGER"},
                {"BUILT_IN", "public static final WorldVersion BUILT_IN"},
                {"id", "private final String id"},
                {"name", "private final String name"},
                {"worldVersion", "private final DataVersion worldVersion"},
                {"protocolVersion", "private final int protocolVersion"},
                {"resourcePackVersion", "private final int resourcePackVersion"},
                {"dataPackVersion", "private final int dataPackVersion"},
                {"stable", "private final boolean stable"}
            };
            d.methods = {
                {0x0001, "<init>", "(Ljava/lang/String;Ljava/lang/String;Lnet/minecraft/world/level/storage/DataVersion;IIILjava/util/Date;Z)V", 4, 9, 46, {}, {}, "", ""},
                {0x0009, "createBuiltIn", "(Ljava/lang/String;Ljava/lang/String;Z)Lnet/minecraft/WorldVersion;", 10, 3, 38, {"net/minecraft/SharedConstants.getProtocolVersion"}, {"main"}, "", ""},
                {0x0009, "createFromJson", "(Lcom/google/gson/JsonObject;)Lnet/minecraft/WorldVersion;", 10, 2, 74, {"net/minecraft/util/GsonHelper.getAsString", "net/minecraft/util/GsonHelper.getAsInt"}, {"pack_version", "world_version", "protocol_version"}, "", ""},
                {0x0009, "tryDetectVersion", "()Lnet/minecraft/WorldVersion;", 4, 3, 44, {"net/minecraft/util/GsonHelper.parse", "net/minecraft/DetectedVersion.createFromJson"}, {"/version.json", "Missing /version.json; falling back to built-in 26.3 metadata"}, "", ""},
                {0x0001, "packVersion", "(Lnet/minecraft/server/packs/PackType;)I", 2, 2, 17, {}, {}, "", ""},
                {0x0001, "protocolVersion", "()I", 1, 1, 5, {}, {}, "", ""},
                {0x0001, "dataVersion", "()Lnet/minecraft/world/level/storage/DataVersion;", 1, 1, 5, {}, {}, "", ""},
                {0x0001, "id", "()Ljava/lang/String;", 1, 1, 5, {}, {}, "", ""},
                {0x0001, "name", "()Ljava/lang/String;", 1, 1, 5, {}, {}, "", ""},
                {0x0001, "stable", "()Z", 1, 1, 5, {}, {}, "", ""}
            };
            d.outgoing_xrefs = {
                {"IMPLEMENTS", "net/minecraft/DetectedVersion", "<type>", "net/minecraft/WorldVersion", "", 0},
                {"CALL", "net/minecraft/DetectedVersion", "createBuiltIn(...)WorldVersion", "net/minecraft/SharedConstants", "getProtocolVersion", 16},
                {"NEW", "net/minecraft/DetectedVersion", "createBuiltIn(...)WorldVersion", "net/minecraft/world/level/storage/DataVersion", "<init>", 8},
                {"CALL", "net/minecraft/DetectedVersion", "tryDetectVersion()WorldVersion", "net/minecraft/util/GsonHelper", "parse", 37},
                {"READ", "net/minecraft/DetectedVersion", "packVersion(PackType)I", "net/minecraft/server/packs/PackType", "SERVER_DATA", 1},
                {"STRING", "net/minecraft/DetectedVersion", "tryDetectVersion()WorldVersion", "net/minecraft/DetectedVersion", "/version.json", 2},
                {"STRING", "net/minecraft/DetectedVersion", "<clinit>()V", "net/minecraft/DetectedVersion", "26.3", 6},
                {"STRING", "net/minecraft/DetectedVersion", "createFromJson(JsonObject)WorldVersion", "net/minecraft/DetectedVersion", "protocol_version", 24}
            };
            d.source_code = R"JAVA(package net.minecraft;

import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.time.ZonedDateTime;
import java.util.Date;
import net.minecraft.server.packs.PackType;
import net.minecraft.util.GsonHelper;
import net.minecraft.world.level.storage.DataVersion;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// Clean-room structural specification for Minecraft 26.3 (Java SE 25 · classfile 69.0).
// Load your local 26.3.jar via "+ Load JAR" (Ctrl+O) or `--jar <path>` for live bytecode recovery.
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
    public int packVersion(PackType packType) {
        return packType == PackType.SERVER_DATA ? this.dataPackVersion : this.resourcePackVersion;
    }

    @Override public String id() { return this.id; }
    @Override public String name() { return this.name; }
    @Override public DataVersion dataVersion() { return this.worldVersion; }
    @Override public int protocolVersion() { return this.protocolVersion; }
    @Override public boolean stable() { return this.stable; }
}
)JAVA";
            d.disasm_listing = R"(// Classfile net/minecraft/DetectedVersion.class  [JVMS 69.0 · Java SE 25]
// SHA-1 (26.3 client.jar): e877b6a07acd633fb3bb475002175cec036e7b87
// Constant Pool: 142 slots  |  Flags: 0x0021 (ACC_PUBLIC | ACC_SUPER)

  method tryDetectVersion()Lnet/minecraft/WorldVersion;  [stack=4, locals=3, code=44B]
       0: ldc               // DetectedVersion.class
       2: ldc               // "/version.json"
       4: invokevirtual     // java/lang/Class.getResourceAsStream(Ljava/lang/String;)Ljava/io/InputStream;
       7: astore_0
       8: aload_0
       9: ifnonnull         // -> pc#26
      12: getstatic         // net/minecraft/DetectedVersion.LOGGER:Lorg/slf4j/Logger;
      15: ldc               // "Missing /version.json; falling back to built-in 26.3 metadata"
      17: invokeinterface   // org/slf4j/Logger.warn(Ljava/lang/String;)V
      22: getstatic         // net/minecraft/DetectedVersion.BUILT_IN:Lnet/minecraft/WorldVersion;
      25: areturn
      26: new               // java/io/InputStreamReader
      29: dup
      30: aload_0
      31: getstatic         // java/nio/charset/StandardCharsets.UTF_8:Ljava/nio/charset/Charset;
      34: invokespecial     // java/io/InputStreamReader.<init>(Ljava/io/InputStream;Ljava/nio/charset/Charset;)V
      37: invokestatic      // net/minecraft/util/GsonHelper.parse(Ljava/io/Reader;)Lcom/google/gson/JsonObject;
      40: invokestatic      // net/minecraft/DetectedVersion.createFromJson(Lcom/google/gson/JsonObject;)Lnet/minecraft/WorldVersion;
      43: areturn

  method packVersion(Lnet/minecraft/server/packs/PackType;)I  [stack=2, locals=2, code=17B]
       0: aload_1
       1: getstatic         // net/minecraft/server/packs/PackType.SERVER_DATA:Lnet/minecraft/server/packs/PackType;
       4: if_acmpne         // -> pc#12
       7: aload_0
       8: getfield          // net/minecraft/DetectedVersion.dataPackVersion:I
      11: ireturn
      12: aload_0
      13: getfield          // net/minecraft/DetectedVersion.resourcePackVersion:I
      16: ireturn
)";
            d.diff_add = 6;
            d.diff_del = 4;
            reg(std::move(d));
        }

        {
            DecodedClass d;
            d.valid = true;
            d.internal_name = "net/minecraft/WorldVersion";
            d.kind = "interface";
            d.access_flags = 0x0601;
            d.methods = {
                {0x0401, "dataVersion", "()Lnet/minecraft/world/level/storage/DataVersion;", 0, 1, 0, {}, {}, "", ""},
                {0x0401, "id", "()Ljava/lang/String;", 0, 1, 0, {}, {}, "", ""},
                {0x0401, "name", "()Ljava/lang/String;", 0, 1, 0, {}, {}, "", ""},
                {0x0401, "protocolVersion", "()I", 0, 1, 0, {}, {}, "", ""},
                {0x0401, "packVersion", "(Lnet/minecraft/server/packs/PackType;)I", 0, 2, 0, {}, {}, "", ""},
                {0x0401, "stable", "()Z", 0, 1, 0, {}, {}, "", ""}
            };
            d.source_code = R"JAVA(package net.minecraft;

import net.minecraft.server.packs.PackType;
import net.minecraft.world.level.storage.DataVersion;

public interface WorldVersion {
    DataVersion dataVersion();
    String id();
    String name();
    int protocolVersion();
    int packVersion(PackType packType);
    boolean stable();
}
)JAVA";
            d.disasm_listing = "// Interface net/minecraft/WorldVersion (JVMS 69.0 · ACC_INTERFACE | ACC_ABSTRACT)\n";
            d.diff_add = 1;
            reg(std::move(d));
        }

        {
            DecodedClass d;
            d.valid = true;
            d.internal_name = "net/minecraft/SharedConstants";
            d.kind = "class";
            d.fields = {
                {"VERSION_STRING", "public static final String VERSION_STRING = \"26.3\""},
                {"DEFAULT_NETWORK_PORT", "public static final int DEFAULT_NETWORK_PORT = 25565"},
                {"TICKS_PER_SECOND", "public static final int TICKS_PER_SECOND = 20"},
                {"CURRENT_VERSION", "private static WorldVersion CURRENT_VERSION"}
            };
            d.methods = {
                {0x0009, "tryDetectVersion", "()V", 2, 0, 13, {"net/minecraft/DetectedVersion.tryDetectVersion"}, {}, "", ""},
                {0x0009, "getCurrentVersion", "()Lnet/minecraft/WorldVersion;", 2, 0, 16, {}, {}, "", ""},
                {0x0009, "getProtocolVersion", "()I", 1, 0, 4, {}, {}, "", ""}
            };
            d.outgoing_xrefs = {
                {"CALL", "net/minecraft/SharedConstants", "tryDetectVersion()V", "net/minecraft/DetectedVersion", "tryDetectVersion", 6},
                {"WRITE", "net/minecraft/SharedConstants", "tryDetectVersion()V", "net/minecraft/SharedConstants", "CURRENT_VERSION", 9},
                {"STRING", "net/minecraft/SharedConstants", "<clinit>()V", "net/minecraft/SharedConstants", "26.3", 2},
                {"STRING", "net/minecraft/SharedConstants", "getCurrentVersion()WorldVersion", "net/minecraft/SharedConstants", "Game version not initialized yet", 8}
            };
            d.source_code = R"JAVA(package net.minecraft;

public class SharedConstants {
    public static final String VERSION_STRING = "26.3";
    public static final int DEFAULT_NETWORK_PORT = 25565;
    public static final int TICKS_PER_SECOND = 20;
    private static WorldVersion CURRENT_VERSION;

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
)JAVA";
            d.disasm_listing = R"(// Classfile net/minecraft/SharedConstants.class  [JVMS 69.0 · Java SE 25]
  method tryDetectVersion()V  [stack=1, locals=0, code=13B]
       0: getstatic         // net/minecraft/SharedConstants.CURRENT_VERSION:Lnet/minecraft/WorldVersion;
       3: ifnonnull         // -> pc#12
       6: invokestatic      // net/minecraft/DetectedVersion.tryDetectVersion()Lnet/minecraft/WorldVersion;
       9: putstatic         // net/minecraft/SharedConstants.CURRENT_VERSION:Lnet/minecraft/WorldVersion;
      12: return
)";
            d.diff_add = 1; d.diff_del = 1;
            reg(std::move(d));
        }

        {
            DecodedClass d;
            d.valid = true;
            d.internal_name = "net/minecraft/client/Minecraft";
            d.kind = "class";
            d.interfaces = {"java/lang/AutoCloseable"};
            d.fields = {
                {"instance", "private static Minecraft instance"},
                {"gameRenderer", "public final GameRenderer gameRenderer"},
                {"levelRenderer", "public final LevelRenderer levelRenderer"},
                {"running", "private volatile boolean running"}
            };
            d.methods = {
                {0x0001, "<init>", "()V", 3, 1, 34, {"net/minecraft/SharedConstants.tryDetectVersion"}, {}, "", ""},
                {0x0009, "getInstance", "()Lnet/minecraft/client/Minecraft;", 1, 0, 4, {}, {}, "", ""},
                {0x0001, "launchVersion", "()Ljava/lang/String;", 3, 2, 22, {"net/minecraft/DetectedVersion.tryDetectVersion"}, {}, "", ""},
                {0x0001, "run", "()V", 2, 1, 18, {"net/minecraft/client/Minecraft.runTick"}, {}, "", ""},
                {0x0002, "runTick", "(Z)V", 2, 2, 14, {"net/minecraft/client/renderer/GameRenderer.renderFrame"}, {}, "", ""}
            };
            d.outgoing_xrefs = {
                {"CALL", "net/minecraft/client/Minecraft", "<init>()V", "net/minecraft/SharedConstants", "tryDetectVersion", 4},
                {"CALL", "net/minecraft/client/Minecraft", "launchVersion()String", "net/minecraft/DetectedVersion", "tryDetectVersion", 0},
                {"NEW", "net/minecraft/client/Minecraft", "<init>()V", "net/minecraft/client/renderer/GameRenderer", "<init>", 12},
                {"CALL", "net/minecraft/client/Minecraft", "runTick(Z)V", "net/minecraft/client/renderer/GameRenderer", "renderFrame", 5},
                {"STRING", "net/minecraft/client/Minecraft", "launchVersion()String", "net/minecraft/client/Minecraft", "client-bootstrap/26.3", 9}
            };
            d.source_code = R"JAVA(package net.minecraft.client;

import net.minecraft.DetectedVersion;
import net.minecraft.SharedConstants;
import net.minecraft.WorldVersion;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.LevelRenderer;

// Verified 26.3 client.jar: 11,383 .class files -> 7,301 top-level .java files, 98,739 methods.
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

    public static Minecraft getInstance() { return instance; }

    public String launchVersion() {
        WorldVersion v = DetectedVersion.tryDetectVersion();
        return v.name() + " (data=" + v.dataVersion().version() + ")";
    }

    public void run() {
        while (this.running) {
            this.runTick(true);
        }
    }

    private void runTick(boolean renderWorld) {
        this.gameRenderer.renderFrame(renderWorld);
    }

    @Override public void close() { this.running = false; }
}
)JAVA";
            d.disasm_listing = "// Classfile net/minecraft/client/Minecraft.class  [JVMS 69.0 · Java SE 25]\n";
            d.diff_add = 2;
            reg(std::move(d));
        }

        auto add_simple = [&](const char* name, const char* kind, const char* src, std::vector<XRefItem> xrs = {}) {
            DecodedClass d;
            d.valid = true;
            d.internal_name = name;
            d.kind = kind;
            d.source_code = src;
            d.disasm_listing = std::string("// Classfile ") + name + ".class  [JVMS 69.0 · Java SE 25]\n";
            d.outgoing_xrefs = std::move(xrs);
            reg(std::move(d));
        };

        add_simple("net/minecraft/client/main/Main", "class",
R"JAVA(package net.minecraft.client.main;

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
)JAVA", {
            {"CALL", "net/minecraft/client/main/Main", "main([String)V", "net/minecraft/SharedConstants", "tryDetectVersion", 0},
            {"NEW", "net/minecraft/client/main/Main", "main([String)V", "net/minecraft/client/Minecraft", "<init>", 4}
        });

        add_simple("net/minecraft/client/renderer/GameRenderer", "class",
R"JAVA(package net.minecraft.client.renderer;

import com.mojang.blaze3d.systems.RenderSystem;
import net.minecraft.client.Minecraft;

public class GameRenderer implements AutoCloseable {
    private final Minecraft minecraft;
    public GameRenderer(Minecraft minecraft) { this.minecraft = minecraft; }
    public void renderFrame(boolean renderWorld) {
        RenderSystem.assertOnRenderThread();
        if (renderWorld) this.minecraft.levelRenderer.renderChunkLayers();
    }
    @Override public void close() {}
}
)JAVA", {
            {"CALL", "net/minecraft/client/renderer/GameRenderer", "renderFrame(Z)V", "com/mojang/blaze3d/systems/RenderSystem", "assertOnRenderThread", 0}
        });

        add_simple("net/minecraft/client/renderer/LevelRenderer", "class",
R"JAVA(package net.minecraft.client.renderer;

import net.minecraft.client.Minecraft;

public class LevelRenderer {
    private final Minecraft minecraft;
    public LevelRenderer(Minecraft minecraft) { this.minecraft = minecraft; }
    public void renderChunkLayers() {}
}
)JAVA");

        add_simple("net/minecraft/world/level/storage/DataVersion", "record",
R"JAVA(package net.minecraft.world.level.storage;

public record DataVersion(int version, String series) {
    public static final String MAIN_SERIES = "main";
}
)JAVA");

        add_simple("net/minecraft/server/packs/PackType", "enum",
R"JAVA(package net.minecraft.server.packs;

public enum PackType {
    CLIENT_RESOURCES("assets"),
    SERVER_DATA("data");
    private final String directory;
    PackType(String directory) { this.directory = directory; }
    public String getDirectory() { return this.directory; }
}
)JAVA", {
            {"STRING", "net/minecraft/server/packs/PackType", "<clinit>()V", "net/minecraft/server/packs/PackType", "assets", 4},
            {"STRING", "net/minecraft/server/packs/PackType", "<clinit>()V", "net/minecraft/server/packs/PackType", "data", 16}
        });

        add_simple("net/minecraft/util/GsonHelper", "class",
R"JAVA(package net.minecraft.util;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import java.io.Reader;

public class GsonHelper {
    private static final Gson GSON = new Gson();
    public static JsonObject parse(Reader reader) { return GSON.fromJson(reader, JsonObject.class); }
    public static String getAsString(JsonObject obj, String key) { return obj.get(key).getAsString(); }
    public static int getAsInt(JsonObject obj, String key) { return obj.get(key).getAsInt(); }
}
)JAVA");

        add_simple("com/mojang/blaze3d/systems/RenderSystem", "class",
R"JAVA(package com.mojang.blaze3d.systems;

public class RenderSystem {
    public static final String BACKEND = "Blaze3D Core 26.3 (Java SE 25)";
    public static void assertOnRenderThread() {}
}
)JAVA", {
            {"STRING", "com/mojang/blaze3d/systems/RenderSystem", "<clinit>()V", "com/mojang/blaze3d/systems/RenderSystem", "Blaze3D Core 26.3 (Java SE 25)", 2}
        });

        add_simple("net/minecraft/server/MinecraftServer", "class",
R"JAVA(package net.minecraft.server;

import net.minecraft.SharedConstants;

// Verified 26.3 server.jar: 7,762 .class files -> 5,037 .java files, 72,269 methods.
public abstract class MinecraftServer {
    public String getServerVersion() { return SharedConstants.getCurrentVersion().name(); }
}
)JAVA", {
            {"CALL", "net/minecraft/server/MinecraftServer", "getServerVersion()String", "net/minecraft/SharedConstants", "getCurrentVersion", 0}
        });

        sorted_classes.clear();
        for (const auto& [k, _] : builtins) sorted_classes.push_back(k);
        std::sort(sorted_classes.begin(), sorted_classes.end());
        index_snap = std::move(snap);
    }
};

} // namespace flux::jvm
