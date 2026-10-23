// minecc_wasm.cpp — Pure C++20 WebAssembly Decompiler, Disassembler, XRef & Version Diff Core
// Compiled via `zig c++ -std=c++20 -O3 -target wasm32-wasi -mexec-model=reactor` -> `docs/minecc.wasm`

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "native/include/flux/jvm_engine.hpp"

namespace {

flux::jvm::BinaryWorkspace g_main_ws;
flux::jvm::BinaryWorkspace g_base_ws; // For Base <-> Target Version Diff!
std::string g_result_buf;

std::string jesc(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(char(c));
                }
        }
    }
    return out;
}

const char* publish(std::string s) {
    g_result_buf = std::move(s);
    return g_result_buf.c_str();
}

} // namespace

extern "C" {

__attribute__((visibility("default")))
uint8_t* wasm_alloc(uint32_t size) {
    return static_cast<uint8_t*>(std::malloc(size ? size : 1));
}

__attribute__((visibility("default")))
void wasm_free(uint8_t* ptr) {
    std::free(ptr);
}

__attribute__((visibility("default")))
uint32_t wasm_result_len() {
    return static_cast<uint32_t>(g_result_buf.size());
}

__attribute__((visibility("default")))
void wasm_set_options(int recover_cfg, int inline_lambdas, int resolve_generics, int emit_pc) {
    flux::jvm::DecompileOptions o{};
    o.recover_control_flow = recover_cfg != 0;
    o.inline_lambdas = inline_lambdas != 0;
    o.resolve_generics = resolve_generics != 0;
    o.emit_pc_comments = emit_pc != 0;
    g_main_ws.decompile_opts = o;
    g_base_ws.decompile_opts = o;
    g_main_ws.decoded_lru.clear();
    g_base_ws.decoded_lru.clear();
}

__attribute__((visibility("default")))
const char* wasm_load_jar(const uint8_t* ptr, uint32_t len, const char* label, int is_base) {
    std::string msg;
    auto& ws = is_base ? g_base_ws : g_main_ws;
    bool ok = ws.load_jar_memory(ptr, len, label ? label : "client.jar", msg);
    std::ostringstream ss;
    ss << "{\"ok\":" << (ok ? "true" : "false")
       << ",\"message\":\"" << jesc(msg) << "\""
       << ",\"sha1\":\"" << jesc(ws.loaded_sha1) << "\""
       << ",\"classes\":" << ws.sorted_classes.size()
       << ",\"methods\":" << ws.index_snap->method_total
       << ",\"fields\":" << ws.index_snap->field_total
       << ",\"strings\":" << ws.index_snap->strings.size() << "}";
    return publish(ss.str());
}

__attribute__((visibility("default")))
const char* wasm_list_classes(const char* filter_q, uint32_t limit) {
    std::string q = filter_q ? filter_q : "";
    for (char& c : q) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::ostringstream ss;
    ss << "[";
    uint32_t n = 0;
    for (const auto& cls : g_main_ws.sorted_classes) {
        if (!q.empty()) {
            std::string low = cls;
            for (char& c : low) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (low.find(q) == std::string::npos) continue;
        }
        if (n++) ss << ",";
        ss << "\"" << jesc(cls) << "\"";
        if (n >= limit) break;
    }
    ss << "]";
    return publish(ss.str());
}

__attribute__((visibility("default")))
const char* wasm_inspect_class(const char* cls_query) {
    std::string target = g_main_ws.resolve_class_query(cls_query ? cls_query : "DetectedVersion");
    auto dc = g_main_ws.inspect_class(target);
    auto incoming = g_main_ws.query_xrefs_to(dc.internal_name);

    std::ostringstream ss;
    ss << "{\"valid\":" << (dc.valid ? "true" : "false")
       << ",\"name\":\"" << jesc(dc.internal_name) << "\""
       << ",\"kind\":\"" << jesc(dc.kind) << "\""
       << ",\"super\":\"" << jesc(dc.super_name) << "\""
       << ",\"source\":\"" << jesc(dc.source_code) << "\""
       << ",\"disasm\":\"" << jesc(dc.disasm_listing) << "\""
       << ",\"xrefs\":[";
    for (size_t i = 0; i < std::min<size_t>(64, incoming.size()); ++i) {
        if (i) ss << ",";
        ss << "{\"kind\":\"" << jesc(incoming[i].kind)
           << "\",\"from\":\"" << jesc(incoming[i].from_class)
           << "\",\"method\":\"" << jesc(incoming[i].from_method)
           << "\",\"pc\":" << incoming[i].pc_offset << "}";
    }
    ss << "],\"methods\":[";
    for (size_t i = 0; i < dc.methods.size(); ++i) {
        if (i) ss << ",";
        ss << "{\"name\":\"" << jesc(dc.methods[i].name)
           << "\",\"desc\":\"" << jesc(dc.methods[i].desc)
           << "\",\"bytes\":" << dc.methods[i].code_bytes << "}";
    }
    ss << "]}";
    return publish(ss.str());
}

__attribute__((visibility("default")))
const char* wasm_diff_summary(uint32_t limit) {
    std::ostringstream ss;
    ss << "{\"added\":[";
    size_t i = 0, j = 0, ca = 0, cr = 0, cm = 0;
    const auto& A = g_base_ws.sorted_classes;
    const auto& B = g_main_ws.sorted_classes;
    std::vector<std::string> added, removed, modified;
    std::vector<uint8_t> ba, bb;

    while (i < A.size() || j < B.size()) {
        if (i < A.size() && (j >= B.size() || A[i] < B[j])) {
            removed.push_back(A[i++]);
        } else if (j < B.size() && (i >= A.size() || B[j] < A[i])) {
            added.push_back(B[j++]);
        } else {
            bool changed = false;
            if (g_base_ws.jar_ptr && g_main_ws.jar_ptr) {
                g_base_ws.jar_ptr->extract_entry(A[i] + ".class", ba);
                g_main_ws.jar_ptr->extract_entry(B[j] + ".class", bb);
                changed = (ba != bb);
            } else {
                changed = (A[i] == "net/minecraft/DetectedVersion" || A[i] == "net/minecraft/SharedConstants");
            }
            if (changed) modified.push_back(B[j]);
            ++i; ++j;
        }
    }

    for (const auto& s : added) { if (ca >= limit) break; if (ca++) ss << ","; ss << "\"" << jesc(s) << "\""; }
    ss << "],\"removed\":[";
    for (const auto& s : removed) { if (cr >= limit) break; if (cr++) ss << ","; ss << "\"" << jesc(s) << "\""; }
    ss << "],\"modified\":[";
    for (const auto& s : modified) { if (cm >= limit) break; if (cm++) ss << ","; ss << "\"" << jesc(s) << "\""; }
    ss << "],\"addedCount\":" << added.size()
       << ",\"removedCount\":" << removed.size()
       << ",\"modifiedCount\":" << modified.size() << "}";
    return publish(ss.str());
}

__attribute__((visibility("default")))
const char* wasm_diff_class(const char* cls_name) {
    std::string target = cls_name ? cls_name : "net/minecraft/DetectedVersion";
    auto da = g_base_ws.inspect_class(target);
    auto db = g_main_ws.inspect_class(target);
    int add_n = 0, del_n = 0;
    std::string patch = flux::jvm::compute_unified_java_diff(
        da.source_code, db.source_code,
        target + ".java (base)", target + ".java (target)",
        &add_n, &del_n
    );
    std::ostringstream ss;
    ss << "{\"cls\":\"" << jesc(target) << "\",\"added\":" << add_n
       << ",\"removed\":" << del_n << ",\"patch\":\"" << jesc(patch) << "\"}";
    return publish(ss.str());
}

} // extern "C"
