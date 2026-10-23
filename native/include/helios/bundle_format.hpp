#pragma once
// Binary container format (.muib = Minecc UI Bytecode Bundle) emitted by `minecc-uic`
// and loaded at zero parse overhead by `minecc-studio` on both Linux & Windows.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace helios::bundle {

inline constexpr char MAGIC[4] = {'M', 'U', 'I', 'B'};
inline constexpr uint32_t FORMAT_VERSION = 1u;

struct BundlePayload {
    std::string entry_name;
    std::string html_source;
    std::string css_source;
    std::string js_source;
    uint32_t dom_node_estimate = 0;
    uint32_t css_rule_count = 0;
    uint64_t fnv1a_checksum = 0;
};

inline uint64_t fnv1a_hash(std::string_view a, std::string_view b, std::string_view c) {
    uint64_t h = 14695981039346656037ull;
    auto feed = [&](std::string_view s) {
        for (unsigned char ch : s) {
            h ^= ch;
            h *= 1099511628211ull;
        }
    };
    feed(a);
    feed(b);
    feed(c);
    return h;
}

inline void write_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

inline void write_u64_le(std::vector<uint8_t>& buf, uint64_t v) {
    write_u32_le(buf, static_cast<uint32_t>(v & 0xFFFFFFFFull));
    write_u32_le(buf, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

inline void write_str(std::vector<uint8_t>& buf, const std::string& s) {
    write_u32_le(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

inline uint32_t read_u32_le(const uint8_t* p, size_t len, size_t& pos) {
    if (pos + 4 > len) throw std::runtime_error("Corrupt .muib bundle (truncated u32)");
    uint32_t v = static_cast<uint32_t>(p[pos]) |
                 (static_cast<uint32_t>(p[pos + 1]) << 8) |
                 (static_cast<uint32_t>(p[pos + 2]) << 16) |
                 (static_cast<uint32_t>(p[pos + 3]) << 24);
    pos += 4;
    return v;
}

inline uint64_t read_u64_le(const uint8_t* p, size_t len, size_t& pos) {
    uint64_t lo = read_u32_le(p, len, pos);
    uint64_t hi = read_u32_le(p, len, pos);
    return lo | (hi << 32);
}

inline std::string read_str(const uint8_t* p, size_t len, size_t& pos) {
    uint32_t n = read_u32_le(p, len, pos);
    if (pos + n > len) throw std::runtime_error("Corrupt .muib bundle (truncated string segment)");
    std::string out(reinterpret_cast<const char*>(p + pos), n);
    pos += n;
    return out;
}

inline std::vector<uint8_t> serialize_bundle(BundlePayload payload) {
    payload.fnv1a_checksum = fnv1a_hash(payload.html_source, payload.css_source, payload.js_source);
    std::vector<uint8_t> out;
    out.insert(out.end(), MAGIC, MAGIC + 4);
    write_u32_le(out, FORMAT_VERSION);
    write_u32_le(out, payload.dom_node_estimate);
    write_u32_le(out, payload.css_rule_count);
    write_u64_le(out, payload.fnv1a_checksum);
    write_str(out, payload.entry_name);
    write_str(out, payload.html_source);
    write_str(out, payload.css_source);
    write_str(out, payload.js_source);
    return out;
}

inline BundlePayload deserialize_bundle(const uint8_t* data, size_t size) {
    if (size < 24 || std::memcmp(data, MAGIC, 4) != 0) {
        throw std::runtime_error("Invalid .muib header magic");
    }
    size_t pos = 4;
    uint32_t ver = read_u32_le(data, size, pos);
    if (ver != FORMAT_VERSION) {
        throw std::runtime_error("Unsupported .muib format version");
    }
    BundlePayload b;
    b.dom_node_estimate = read_u32_le(data, size, pos);
    b.css_rule_count = read_u32_le(data, size, pos);
    b.fnv1a_checksum = read_u64_le(data, size, pos);
    b.entry_name = read_str(data, size, pos);
    b.html_source = read_str(data, size, pos);
    b.css_source = read_str(data, size, pos);
    b.js_source = read_str(data, size, pos);

    uint64_t verify = fnv1a_hash(b.html_source, b.css_source, b.js_source);
    if (verify != b.fnv1a_checksum) {
        throw std::runtime_error("Checksum mismatch in .muib bundle");
    }
    return b;
}

} // namespace helios::bundle
