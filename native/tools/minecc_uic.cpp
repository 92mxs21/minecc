// minecc-uic v2.0: Minimal Forked HTML + CSS Design Token & Layout AOT Compiler
// Compiles `ui/flux_theme.css` + `ui/flux_layout.html` directly into zero-allocation C++20 `constexpr`
// headers (`native/generated/flux_ui_spec.hpp`) — zero runtime DOM bloat, zero JS interpreter lag.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim(std::string_view sv) {
    size_t a = 0, b = sv.size();
    while (a < b && std::isspace(static_cast<unsigned char>(sv[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) --b;
    return std::string(sv.substr(a, b - a));
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open UI source file: " + p.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string unescape_entities(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s.substr(i, 5) == "&amp;") { out += "&"; i += 5; }
        else if (s.substr(i, 4) == "&lt;") { out += "<"; i += 4; }
        else if (s.substr(i, 4) == "&gt;") { out += ">"; i += 4; }
        else if (s.substr(i, 6) == "&quot;") { out += "\""; i += 6; }
        else out.push_back(s[i++]);
    }
    return out;
}

uint32_t parse_css_color(std::string s) {
    s = trim(s);
    if (!s.empty() && s[0] == '#' && s.size() == 7) {
        uint32_t rgb = static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16));
        return 0xFF000000u | rgb;
    }
    if (s.rfind("rgba(", 0) == 0) {
        size_t l = s.find('('), r = s.find(')', l);
        std::stringstream ss(s.substr(l + 1, r - l - 1));
        std::string item;
        std::vector<float> v;
        while (std::getline(ss, item, ',')) v.push_back(std::strtof(item.c_str(), nullptr));
        if (v.size() >= 4) {
            uint32_t cr = static_cast<uint32_t>(std::clamp(v[0], 0.0f, 255.0f));
            uint32_t cg = static_cast<uint32_t>(std::clamp(v[1], 0.0f, 255.0f));
            uint32_t cb = static_cast<uint32_t>(std::clamp(v[2], 0.0f, 255.0f));
            uint32_t ca = static_cast<uint32_t>(std::clamp(v[3] <= 1.0f ? v[3] * 255.0f : v[3], 0.0f, 255.0f));
            return (ca << 24) | (cr << 16) | (cg << 8) | cb;
        }
    }
    return 0xFF0F131Bu;
}

std::string cpp_str(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out.push_back(static_cast<char>(c));
    }
    out += "\"";
    return out;
}

struct TagNode {
    std::string tag;
    std::map<std::string, std::string> attrs;
};

std::vector<TagNode> parse_minimal_html(std::string_view src) {
    std::vector<TagNode> nodes;
    size_t i = 0, n = src.size();
    while (i < n) {
        size_t lt = src.find('<', i);
        if (lt == std::string_view::npos) break;
        if (src.substr(lt, 4) == "<!--") {
            size_t end = src.find("-->", lt + 4);
            i = (end == std::string_view::npos) ? n : end + 3;
            continue;
        }
        if (lt + 1 < n && (src[lt + 1] == '!' || src[lt + 1] == '/')) {
            size_t gt = src.find('>', lt + 2);
            i = (gt == std::string_view::npos) ? n : gt + 1;
            continue;
        }
        size_t cur = lt + 1;
        while (cur < n && !std::isspace(static_cast<unsigned char>(src[cur])) && src[cur] != '>' && src[cur] != '/') ++cur;
        TagNode node;
        node.tag = std::string(src.substr(lt + 1, cur - lt - 1));
        while (cur < n && src[cur] != '>') {
            while (cur < n && (std::isspace(static_cast<unsigned char>(src[cur])) || src[cur] == '/')) ++cur;
            if (cur >= n || src[cur] == '>') break;
            size_t ks = cur;
            while (cur < n && src[cur] != '=' && !std::isspace(static_cast<unsigned char>(src[cur])) && src[cur] != '>') ++cur;
            std::string key(src.substr(ks, cur - ks));
            while (cur < n && std::isspace(static_cast<unsigned char>(src[cur]))) ++cur;
            std::string val;
            if (cur < n && src[cur] == '=') {
                ++cur;
                while (cur < n && std::isspace(static_cast<unsigned char>(src[cur]))) ++cur;
                if (cur < n && (src[cur] == '"' || src[cur] == '\'')) {
                    char q = src[cur++];
                    size_t vs = cur;
                    while (cur < n && src[cur] != q) ++cur;
                    val = unescape_entities(src.substr(vs, cur - vs));
                    if (cur < n) ++cur;
                }
            }
            if (!key.empty()) node.attrs[key] = val;
        }
        nodes.push_back(std::move(node));
        i = (cur < n) ? cur + 1 : n;
    }
    return nodes;
}

std::map<std::string, std::string> parse_minimal_css_tokens(std::string_view css) {
    std::map<std::string, std::string> vars;
    std::string clean;
    for (size_t i = 0; i < css.size();) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            size_t e = css.find("*/", i + 2);
            i = (e == std::string_view::npos) ? css.size() : e + 2;
        } else {
            clean.push_back(css[i++]);
        }
    }
    size_t pos = 0;
    while ((pos = clean.find("--", pos)) != std::string::npos) {
        size_t colon = clean.find(':', pos);
        size_t semi = clean.find(';', colon == std::string::npos ? pos : colon);
        if (colon == std::string::npos || semi == std::string::npos) break;
        vars[trim(std::string_view(clean).substr(pos, colon - pos))] =
            trim(std::string_view(clean).substr(colon + 1, semi - colon - 1));
        pos = semi + 1;
    }
    return vars;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "minecc-uic v2.0 — Minimal HTML+CSS Design Subset AOT Compiler\n"
                  << "Usage: minecc-uic <flux_layout.html> [--css <flux_theme.css>] --emit-cpp <flux_ui_spec.hpp>\n";
        return 1;
    }

    fs::path html_path;
    fs::path css_path;
    fs::path out_hpp;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "minecc-uic 2.0.0 (FluxUI Minimal HTML+CSS AOT Compiler)\n";
            return 0;
        } else if (a == "--css" && i + 1 < argc) css_path = argv[++i];
        else if (a == "--emit-cpp" && i + 1 < argc) out_hpp = argv[++i];
        else if (html_path.empty()) html_path = a;
    }

    try {
        if (css_path.empty()) css_path = html_path.parent_path() / "flux_theme.css";
        if (out_hpp.empty()) out_hpp = "native/generated/flux_ui_spec.hpp";

        auto tags = parse_minimal_html(read_file(html_path));
        auto vars = parse_minimal_css_tokens(read_file(css_path));

        auto col = [&](const char* name, uint32_t def) -> uint32_t {
            auto it = vars.find(name);
            return it == vars.end() ? def : parse_css_color(it->second);
        };
        auto num = [&](const char* name, float def) -> float {
            auto it = vars.find(name);
            return it == vars.end() ? def : std::strtof(it->second.c_str(), nullptr);
        };

        TagNode app_tag, hero_tag;
        std::vector<TagNode> bentos;
        for (const auto& t : tags) {
            if (t.tag == "app") app_tag = t;
            else if (t.tag == "hero") hero_tag = t;
            else if (t.tag == "bento") bentos.push_back(t);
        }

        if (out_hpp.has_parent_path()) fs::create_directories(out_hpp.parent_path());
        std::ofstream out(out_hpp);
        if (!out) throw std::runtime_error("Cannot write C++ spec header: " + out_hpp.string());

        out << "#pragma once\n"
            << "// Auto-generated by minecc-uic v2.0 from " << html_path.filename().string()
            << " + " << css_path.filename().string() << "\n"
            << "#include <cstdint>\n\n"
            << "namespace flux::spec {\n\n"
            << "struct ThemePalette {\n"
            << "    uint32_t bg_abyss, bg_rail, bg_topbar, bg_panel, bg_card, bg_card_alt, bg_card_hover, bg_elevated;\n"
            << "    uint32_t bg_selected, bg_line_hl, border_subtle, border_strong, border_active, glass_highlight;\n"
            << "    uint32_t accent_cyan, accent_indigo, accent_emerald, accent_amber, accent_rose;\n"
            << "    uint32_t text_bright, text_primary, text_secondary, text_muted;\n"
            << "    uint32_t syn_kw, syn_type, syn_method, syn_field, syn_str, syn_num, syn_cmt, syn_annot;\n"
            << "    float rail_w, topbar_h, radius_sm, radius_md, radius_lg, anim_speed;\n"
            << "};\n\n"
            << std::hex << std::showbase
            << "inline constexpr ThemePalette THEME = {\n"
            << "    " << col("--bg-abyss", 0xFF07090Du) << "u, " << col("--bg-rail", 0xFF0A0D13u) << "u, "
            << col("--bg-topbar", 0xFF0C1017u) << "u, " << col("--bg-panel", 0xFF0E131Bu) << "u,\n"
            << "    " << col("--bg-card", 0xFF121824u) << "u, " << col("--bg-card-alt", 0xFF0F1520u) << "u, "
            << col("--bg-card-hover", 0xFF182132u) << "u, " << col("--bg-elevated", 0xFF1B2538u) << "u,\n"
            << "    " << col("--bg-selected", 0x2438BDF8u) << "u, " << col("--bg-line-hl", 0x1738BDF8u) << "u, "
            << col("--border-subtle", 0x1C94A3B8u) << "u, " << col("--border-strong", 0x3394A3B8u) << "u,\n"
            << "    " << col("--border-active", 0x8538BDF8u) << "u, " << col("--glass-highlight", 0x0DFFFFFFu) << "u,\n"
            << "    " << col("--accent-cyan", 0xFF38BDF8u) << "u, " << col("--accent-indigo", 0xFF818CF8u) << "u, "
            << col("--accent-emerald", 0xFF34D399u) << "u, " << col("--accent-amber", 0xFFFBBF24u) << "u, "
            << col("--accent-rose", 0xFFFB7185u) << "u,\n"
            << "    " << col("--text-bright", 0xFFF8FAFCu) << "u, " << col("--text-primary", 0xFFCBD5E1u) << "u, "
            << col("--text-secondary", 0xFF94A3B8u) << "u, " << col("--text-muted", 0xFF5B6B82u) << "u,\n"
            << "    " << col("--syn-keyword", 0xFFC084FCu) << "u, " << col("--syn-type", 0xFF38BDF8u) << "u, "
            << col("--syn-method", 0xFFFCD34Du) << "u, " << col("--syn-field", 0xFF7DD3FCu) << "u,\n"
            << "    " << col("--syn-string", 0xFF6EE7B7u) << "u, " << col("--syn-number", 0xFFFB923Cu) << "u, "
            << col("--syn-comment", 0xFF4E6078u) << "u, " << col("--syn-annot", 0xFFF472B6u) << "u,\n"
            << std::dec << std::fixed << std::setprecision(1)
            << "    " << num("--rail-width", 56.0f) << "f, " << num("--topbar-height", 46.0f) << "f, "
            << num("--radius-sm", 6.0f) << "f, " << num("--radius-md", 10.0f) << "f, "
            << num("--radius-lg", 14.0f) << "f, " << num("--anim-speed", 14.0f) << "f\n"
            << "};\n\n"
            << "inline constexpr const char* APP_BRAND = " << cpp_str(app_tag.attrs["brand"]) << ";\n"
            << "inline constexpr const char* APP_BADGE = " << cpp_str(app_tag.attrs["badge"]) << ";\n"
            << "inline constexpr const char* HERO_EYEBROW = " << cpp_str(hero_tag.attrs["eyebrow"]) << ";\n"
            << "inline constexpr const char* HERO_TITLE = " << cpp_str(hero_tag.attrs["title"]) << ";\n"
            << "inline constexpr const char* HERO_LEAD = " << cpp_str(hero_tag.attrs["lead"]) << ";\n"
            << "inline constexpr const char* SEARCH_HINT = " << cpp_str(hero_tag.attrs["search_placeholder"]) << ";\n\n"
            << "struct BentoCardSpec {\n"
            << "    const char* action;\n"
            << "    const char* accent;\n"
            << "    const char* key_hint;\n"
            << "    const char* title;\n"
            << "    const char* desc;\n"
            << "};\n\n"
            << "inline constexpr size_t BENTO_COUNT = " << bentos.size() << "u;\n"
            << "inline constexpr BentoCardSpec BENTO_CARDS[" << std::max<size_t>(1, bentos.size()) << "] = {\n";
        for (auto& b : bentos) {
            out << "    {" << cpp_str(b.attrs["action"]) << ", " << cpp_str(b.attrs["accent"]) << ", "
                << cpp_str(b.attrs["key"]) << ", " << cpp_str(b.attrs["title"]) << ", "
                << cpp_str(b.attrs["desc"]) << "},\n";
        }
        out << "};\n\n} // namespace flux::spec\n";

        std::cout << "[minecc-uic] Compiled '" << html_path.filename().string() << "' + '"
                  << css_path.filename().string() << "' -> " << vars.size() << " CSS tokens, "
                  << bentos.size() << " Bento cards -> " << out_hpp.string() << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[minecc-uic] Error: " << ex.what() << "\n";
        return 2;
    }
}
