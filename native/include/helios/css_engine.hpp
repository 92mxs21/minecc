#pragma once
// HeliosUI CSS3 Cascade, Custom Properties (var(--*)), Specificity & Flexbox Layout Engine

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "native/generated/font_atlas.hpp"
#include "native/include/helios/html_dom.hpp"

namespace helios {

struct SimpleSelector {
    std::string tag; // "" or "*" for wildcard
    std::string id;
    std::vector<std::string> classes;
    std::vector<std::pair<std::string, std::string>> attr_equals;
    bool pseudo_root = false;
    bool pseudo_hover = false;
    bool pseudo_active = false;
    bool pseudo_focus = false;
};

struct SelectorChain {
    // Ordered root -> target (descendant combinator ` ` or child combinator `>`)
    std::vector<SimpleSelector> parts;
    std::vector<char> combinators; // ' ' or '>'
    uint32_t specificity = 0;      // packed (id<<16) | (cls<<8) | tag
};

struct CssDeclaration {
    std::string prop;
    std::string value;
    bool important = false;
};

struct CssRule {
    SelectorChain selector;
    std::vector<CssDeclaration> decls;
    uint32_t order = 0;
};

struct Stylesheet {
    std::vector<CssRule> rules;
};

namespace css_detail {

inline std::string trim(std::string_view sv) {
    size_t a = 0;
    size_t b = sv.size();
    while (a < b && std::isspace(static_cast<unsigned char>(sv[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) --b;
    return std::string(sv.substr(a, b - a));
}

inline std::string strip_css_comments(std::string_view css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            size_t end = css.find("*/", i + 2);
            if (end == std::string_view::npos) break;
            i = end + 2;
            continue;
        }
        out.push_back(css[i++]);
    }
    return out;
}

inline SimpleSelector parse_simple_selector(std::string_view tok, uint32_t& id_n, uint32_t& cls_n, uint32_t& tag_n) {
    SimpleSelector sel;
    size_t i = 0;
    const size_t n = tok.size();
    while (i < n) {
        char c = tok[i];
        if (c == '#') {
            ++i;
            size_t s = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(tok[i])) || tok[i] == '-' || tok[i] == '_')) ++i;
            sel.id = std::string(tok.substr(s, i - s));
            ++id_n;
        } else if (c == '.') {
            ++i;
            size_t s = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(tok[i])) || tok[i] == '-' || tok[i] == '_')) ++i;
            sel.classes.emplace_back(tok.substr(s, i - s));
            ++cls_n;
        } else if (c == '[') {
            ++i;
            size_t close = tok.find(']', i);
            std::string_view body = (close == std::string_view::npos) ? tok.substr(i) : tok.substr(i, close - i);
            size_t eq = body.find('=');
            if (eq != std::string_view::npos) {
                std::string k = detail::to_lower_ascii(trim(body.substr(0, eq)));
                std::string v = trim(body.substr(eq + 1));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
                sel.attr_equals.emplace_back(std::move(k), std::move(v));
            } else {
                sel.attr_equals.emplace_back(detail::to_lower_ascii(trim(body)), "*");
            }
            ++cls_n;
            i = (close == std::string_view::npos) ? n : close + 1;
        } else if (c == ':') {
            ++i;
            if (i < n && tok[i] == ':') ++i;
            size_t s = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(tok[i])) || tok[i] == '-')) ++i;
            std::string ps = detail::to_lower_ascii(tok.substr(s, i - s));
            if (ps == "root") sel.pseudo_root = true;
            else if (ps == "hover") sel.pseudo_hover = true;
            else if (ps == "active") sel.pseudo_active = true;
            else if (ps == "focus") sel.pseudo_focus = true;
            ++cls_n;
        } else {
            size_t s = i;
            while (i < n && tok[i] != '#' && tok[i] != '.' && tok[i] != '[' && tok[i] != ':') ++i;
            sel.tag = detail::to_lower_ascii(tok.substr(s, i - s));
            if (!sel.tag.empty() && sel.tag != "*") ++tag_n;
        }
    }
    return sel;
}

inline SelectorChain parse_selector_chain(std::string_view raw) {
    SelectorChain chain;
    uint32_t id_n = 0, cls_n = 0, tag_n = 0;
    std::string s = trim(raw);
    size_t i = 0;
    const size_t n = s.size();
    char pending_comb = ' ';

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i < n && s[i] == '>') {
            pending_comb = '>';
            ++i;
            while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        }
        size_t start = i;
        int bracket_depth = 0;
        while (i < n) {
            if (s[i] == '[') ++bracket_depth;
            else if (s[i] == ']') bracket_depth = std::max(0, bracket_depth - 1);
            else if (bracket_depth == 0 && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == '>')) break;
            ++i;
        }
        if (i > start) {
            if (!chain.parts.empty()) chain.combinators.push_back(pending_comb);
            chain.parts.push_back(parse_simple_selector(std::string_view(s).substr(start, i - start), id_n, cls_n, tag_n));
            pending_comb = ' ';
        }
    }
    chain.specificity = (std::min(id_n, 255u) << 16) | (std::min(cls_n, 255u) << 8) | std::min(tag_n, 255u);
    return chain;
}

inline std::vector<CssDeclaration> parse_declarations(std::string_view block) {
    std::vector<CssDeclaration> out;
    size_t start = 0;
    while (start < block.size()) {
        size_t semi = block.find(';', start);
        std::string_view item = (semi == std::string_view::npos) ? block.substr(start) : block.substr(start, semi - start);
        size_t colon = item.find(':');
        if (colon != std::string_view::npos) {
            std::string prop = trim(item.substr(0, colon));
            if (prop.rfind("--", 0) != 0) prop = detail::to_lower_ascii(prop);
            std::string val = trim(item.substr(colon + 1));
            bool imp = false;
            size_t bang = val.rfind("!important");
            if (bang != std::string::npos) {
                imp = true;
                val = trim(std::string_view(val).substr(0, bang));
            }
            if (!prop.empty() && !val.empty()) out.push_back({std::move(prop), std::move(val), imp});
        }
        if (semi == std::string_view::npos) break;
        start = semi + 1;
    }
    return out;
}

inline bool matches_simple(const DomNode& node, const SimpleSelector& s) {
    if (node.type != NodeType::Element) return false;
    if (s.pseudo_root && node.tag != "html") return false;
    if (!s.tag.empty() && s.tag != "*" && node.tag != s.tag) return false;
    if (!s.id.empty() && node.id != s.id) return false;
    if (s.pseudo_hover && !node.hovered) return false;
    if (s.pseudo_active && !node.active) return false;
    if (s.pseudo_focus && !node.focused) return false;
    for (const auto& cls : s.classes) {
        if (!node.has_class(cls)) return false;
    }
    for (const auto& [ak, av] : s.attr_equals) {
        auto it = node.attrs.find(ak);
        if (it == node.attrs.end()) return false;
        if (av != "*" && it->second != av) return false;
    }
    return true;
}

inline bool matches_chain(const DomNode* node, const SelectorChain& ch) {
    if (ch.parts.empty() || !node) return false;
    int idx = static_cast<int>(ch.parts.size()) - 1;
    if (!matches_simple(*node, ch.parts[idx])) return false;

    const DomNode* cur = node;
    while (idx > 0) {
        char comb = ch.combinators[idx - 1];
        auto p = cur->parent.lock();
        if (!p) return false;
        if (comb == '>') {
            if (!matches_simple(*p, ch.parts[idx - 1])) return false;
            cur = p.get();
            --idx;
        } else {
            bool found = false;
            while (p) {
                if (matches_simple(*p, ch.parts[idx - 1])) {
                    cur = p.get();
                    found = true;
                    break;
                }
                p = p->parent.lock();
            }
            if (!found) return false;
            --idx;
        }
    }
    return true;
}

inline std::string resolve_vars(const std::string& raw, const std::unordered_map<std::string, std::string>& vars, int depth = 0) {
    if (depth > 8 || raw.find("var(") == std::string::npos) return raw;
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        size_t pos = raw.find("var(", i);
        if (pos == std::string::npos) {
            out += raw.substr(i);
            break;
        }
        out += raw.substr(i, pos - i);
        int paren = 1;
        size_t cur = pos + 4;
        while (cur < raw.size() && paren > 0) {
            if (raw[cur] == '(') ++paren;
            else if (raw[cur] == ')') --paren;
            ++cur;
        }
        std::string inside = trim(std::string_view(raw).substr(pos + 4, cur - pos - 5));
        size_t comma = inside.find(',');
        std::string var_name = trim(comma == std::string::npos ? inside : inside.substr(0, comma));
        std::string fallback = comma == std::string::npos ? "" : trim(inside.substr(comma + 1));
        auto it = vars.find(var_name);
        if (it != vars.end()) out += resolve_vars(it->second, vars, depth + 1);
        else out += resolve_vars(fallback, vars, depth + 1);
        i = cur;
    }
    return out;
}

inline uint32_t parse_color(const std::string& raw) {
    std::string s = detail::to_lower_ascii(trim(raw));
    if (s.empty() || s == "transparent" || s == "none") return 0x00000000u;
    if (s == "white") return 0xFFFFFFFFu;
    if (s == "black") return 0xFF000000u;
    if (s[0] == '#') {
        auto hex = [](char c) -> uint32_t {
            if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
            return 0u;
        };
        if (s.size() == 4) {
            uint32_t r = hex(s[1]) * 17u, g = hex(s[2]) * 17u, b = hex(s[3]) * 17u;
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        if (s.size() == 7) {
            uint32_t v = static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16));
            return 0xFF000000u | v;
        }
        if (s.size() == 9) {
            uint32_t v = static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16));
            uint32_t rgb = v >> 8;
            uint32_t a = v & 0xFFu;
            return (a << 24) | rgb;
        }
    }
    if (s.rfind("rgba(", 0) == 0 || s.rfind("rgb(", 0) == 0) {
        size_t l = s.find('(');
        size_t r = s.find(')', l);
        if (l != std::string::npos && r != std::string::npos) {
            std::string body = s.substr(l + 1, r - l - 1);
            std::vector<float> nums;
            std::stringstream ss(body);
            std::string item;
            while (std::getline(ss, item, ',')) {
                nums.push_back(std::strtof(item.c_str(), nullptr));
            }
            if (nums.size() >= 3) {
                uint32_t cr = static_cast<uint32_t>(std::clamp(nums[0], 0.0f, 255.0f));
                uint32_t cg = static_cast<uint32_t>(std::clamp(nums[1], 0.0f, 255.0f));
                uint32_t cb = static_cast<uint32_t>(std::clamp(nums[2], 0.0f, 255.0f));
                uint32_t ca = 255u;
                if (nums.size() >= 4) {
                    float a = nums[3];
                    ca = static_cast<uint32_t>(std::clamp(a <= 1.0f ? a * 255.0f : a, 0.0f, 255.0f));
                }
                return (ca << 24) | (cr << 16) | (cg << 8) | cb;
            }
        }
    }
    return 0x00000000u;
}

inline float parse_length(const std::string& s, float viewport_w, float viewport_h, bool allow_pct = true) {
    std::string v = detail::to_lower_ascii(trim(s));
    if (v == "auto" || v.empty()) return -2.0f;
    char* endp = nullptr;
    float num = std::strtof(v.c_str(), &endp);
    std::string unit = endp ? trim(endp) : "";
    if (unit == "%") return allow_pct ? (-1000.0f - num) : num;
    if (unit == "rem" || unit == "em") return num * 14.0f;
    if (unit == "vw") return viewport_w * (num / 100.0f);
    if (unit == "vh") return viewport_h * (num / 100.0f);
    return num;
}

inline void parse_box_quad(const std::string& raw, float vw, float vh, float& t, float& r, float& b, float& l) {
    std::stringstream ss(raw);
    std::vector<float> vals;
    std::string tok;
    while (ss >> tok) vals.push_back(std::max(0.0f, parse_length(tok, vw, vh, false)));
    if (vals.size() == 1) { t = r = b = l = vals[0]; }
    else if (vals.size() == 2) { t = b = vals[0]; r = l = vals[1]; }
    else if (vals.size() == 3) { t = vals[0]; r = l = vals[1]; b = vals[2]; }
    else if (vals.size() >= 4) { t = vals[0]; r = vals[1]; b = vals[2]; l = vals[3]; }
}

inline void parse_border_shorthand(const std::string& val, float vw, float vh, float& width_out, uint32_t& color_out) {
    if (val == "none" || val == "0") {
        width_out = 0.0f;
        color_out = 0u;
        return;
    }
    // Extract e.g. `1px solid #303030` or `1px solid rgba(...)`
    size_t p1 = val.find(' ');
    if (p1 == std::string::npos) return;
    width_out = std::max(0.0f, parse_length(val.substr(0, p1), vw, vh, false));
    size_t p2 = val.find(' ', p1 + 1);
    if (p2 != std::string::npos) {
        color_out = parse_color(val.substr(p2 + 1));
    }
}

} // namespace css_detail

inline Stylesheet parse_stylesheet(std::string_view css_text) {
    Stylesheet sheet;
    std::string clean = css_detail::strip_css_comments(css_text);
    size_t i = 0;
    const size_t n = clean.size();
    uint32_t order = 0;

    while (i < n) {
        size_t lbrace = clean.find('{', i);
        if (lbrace == std::string::npos) break;
        std::string sel_group = css_detail::trim(std::string_view(clean).substr(i, lbrace - i));
        size_t rbrace = clean.find('}', lbrace + 1);
        if (rbrace == std::string::npos) break;
        std::string body = clean.substr(lbrace + 1, rbrace - lbrace - 1);

        auto decls = css_detail::parse_declarations(body);
        size_t s_pos = 0;
        while (s_pos <= sel_group.size()) {
            size_t comma = sel_group.find(',', s_pos);
            std::string single = (comma == std::string::npos)
                                     ? sel_group.substr(s_pos)
                                     : sel_group.substr(s_pos, comma - s_pos);
            single = css_detail::trim(single);
            if (!single.empty() && single[0] != '@') {
                CssRule rule;
                rule.selector = css_detail::parse_selector_chain(single);
                rule.decls = decls;
                rule.order = ++order;
                sheet.rules.push_back(std::move(rule));
            }
            if (comma == std::string::npos) break;
            s_pos = comma + 1;
        }
        i = rbrace + 1;
    }
    return sheet;
}

inline uint32_t decode_next_utf8(std::string_view s, size_t& i) {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80u) { ++i; return c0; }
    if ((c0 & 0xE0u) == 0xC0u && i + 1 < s.size()) {
        uint32_t cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((c0 & 0xF0u) == 0xE0u && i + 2 < s.size()) {
        uint32_t cp = ((c0 & 0x0Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((c0 & 0xF8u) == 0xF0u && i + 3 < s.size()) {
        uint32_t cp = ((c0 & 0x07u) << 18) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return '?';
}

inline std::pair<float, float> measure_text_run(std::string_view text, const ComputedStyle& st) {
    int face = fonts::select_face(st.font_mono, st.font_bold, st.font_px);
    const auto& f = fonts::FACES[face];
    float scale = static_cast<float>(st.font_px) / static_cast<float>(f.px_size);
    float w = 0.0f;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = decode_next_utf8(text, i);
        if (cp == '\n') continue;
        if (const auto* g = fonts::lookup_glyph(face, cp)) {
            w += static_cast<float>(g->advance) * scale;
        }
    }
    return {std::ceil(w), std::ceil(static_cast<float>(f.line_height) * scale)};
}

inline void compute_styles_recursive(
    const std::shared_ptr<DomNode>& node,
    const Stylesheet& sheet,
    float vw,
    float vh)
{
    if (!node) return;
    if (auto p = node->parent.lock()) {
        node->css_vars = p->css_vars;
        ComputedStyle inherited;
        inherited.fg_color = p->style.fg_color;
        inherited.font_mono = p->style.font_mono;
        inherited.font_bold = p->style.font_bold;
        inherited.font_px = p->style.font_px;
        inherited.nowrap = p->style.nowrap;
        inherited.cursor_pointer = p->style.cursor_pointer;
        node->style = inherited;
    } else {
        node->style = ComputedStyle{};
    }

    if (node->type == NodeType::Element) {
        if (node->tag == "b" || node->tag == "strong" || node->tag == "h1" || node->tag == "h2") {
            node->style.font_bold = true;
        }
        if (node->tag == "code" || node->tag == "pre") {
            node->style.font_mono = true;
        }
        if (node->tag == "button" || node->tag == "a") {
            node->style.cursor_pointer = true;
        }

        struct MatchedDecl {
            uint64_t rank;
            const CssDeclaration* decl;
        };
        std::vector<MatchedDecl> matched;
        for (const auto& rule : sheet.rules) {
            if (css_detail::matches_chain(node.get(), rule.selector)) {
                for (const auto& d : rule.decls) {
                    uint64_t rank = (static_cast<uint64_t>(d.important ? 2 : 0) << 56) |
                                    (static_cast<uint64_t>(rule.selector.specificity) << 24) |
                                    static_cast<uint64_t>(rule.order);
                    matched.push_back({rank, &d});
                }
            }
        }
        std::vector<CssDeclaration> inline_decls;
        auto inline_it = node->attrs.find("style");
        if (inline_it != node->attrs.end() && !inline_it->second.empty()) {
            inline_decls = css_detail::parse_declarations(inline_it->second);
            uint32_t ord = 0;
            for (const auto& d : inline_decls) {
                uint64_t rank = (static_cast<uint64_t>(d.important ? 3 : 1) << 56) | (++ord);
                matched.push_back({rank, &d});
            }
        }

        std::sort(matched.begin(), matched.end(), [](const MatchedDecl& a, const MatchedDecl& b) {
            return a.rank < b.rank;
        });

        // Pass 1: register CSS variables (`--*`)
        for (const auto& m : matched) {
            if (m.decl->prop.rfind("--", 0) == 0) {
                node->css_vars[m.decl->prop] = m.decl->value;
            }
        }

        // Pass 2: resolve properties using cascaded CSS variables
        auto& st = node->style;
        for (const auto& m : matched) {
            const std::string& p = m.decl->prop;
            if (p.rfind("--", 0) == 0) continue;
            std::string v = css_detail::trim(css_detail::resolve_vars(m.decl->value, node->css_vars));
            if (v.empty()) continue;

            if (p == "display") {
                st.display_none = (v == "none");
                st.display_flex = (v == "flex" || v == "inline-flex");
            } else if (p == "flex-direction") {
                st.flex_column = (v == "column");
            } else if (p == "flex") {
                if (v == "none") {
                    st.flex_grow = 0.0f; st.flex_shrink = 0.0f; st.flex_basis = -1.0f;
                } else {
                    std::stringstream ss(v);
                    std::string t1, t2, t3;
                    ss >> t1 >> t2 >> t3;
                    if (!t1.empty() && std::isdigit(static_cast<unsigned char>(t1[0]))) {
                        st.flex_grow = std::strtof(t1.c_str(), nullptr);
                        if (!t2.empty()) st.flex_shrink = std::strtof(t2.c_str(), nullptr);
                        if (!t3.empty()) st.flex_basis = css_detail::parse_length(t3, vw, vh, false);
                        else if (st.flex_grow > 0.0f) st.flex_basis = 0.0f;
                    }
                }
            } else if (p == "flex-grow") {
                st.flex_grow = std::strtof(v.c_str(), nullptr);
            } else if (p == "flex-shrink") {
                st.flex_shrink = std::strtof(v.c_str(), nullptr);
            } else if (p == "justify-content") {
                if (v == "center") st.justify = 1;
                else if (v == "flex-end" || v == "end") st.justify = 2;
                else if (v == "space-between") st.justify = 3;
                else st.justify = 0;
            } else if (p == "align-items") {
                if (v == "flex-start" || v == "start") st.align = 1;
                else if (v == "center") st.align = 2;
                else if (v == "flex-end" || v == "end") st.align = 3;
                else st.align = 0;
            } else if (p == "gap") {
                st.gap = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            } else if (p == "width") {
                st.width = css_detail::parse_length(v, vw, vh);
            } else if (p == "height") {
                st.height = css_detail::parse_length(v, vw, vh);
            } else if (p == "min-width") {
                float mw = css_detail::parse_length(v, vw, vh, false);
                st.min_width = mw < 0.0f ? 0.0f : mw;
            } else if (p == "max-width") {
                float mw = css_detail::parse_length(v, vw, vh, false);
                if (mw >= 0.0f) st.max_width = mw;
            } else if (p == "min-height") {
                float mh = css_detail::parse_length(v, vw, vh, false);
                st.min_height = mh < 0.0f ? 0.0f : mh;
            } else if (p == "max-height") {
                float mh = css_detail::parse_length(v, vw, vh, false);
                if (mh >= 0.0f) st.max_height = mh;
            } else if (p == "padding") {
                css_detail::parse_box_quad(v, vw, vh, st.pad_t, st.pad_r, st.pad_b, st.pad_l);
            } else if (p == "padding-top") st.pad_t = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "padding-right") st.pad_r = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "padding-bottom") st.pad_b = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "padding-left") st.pad_l = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "padding-inline") {
                float pv = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
                st.pad_l = st.pad_r = pv;
            } else if (p == "margin") {
                css_detail::parse_box_quad(v, vw, vh, st.mar_t, st.mar_r, st.mar_b, st.mar_l);
            } else if (p == "margin-top") st.mar_t = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "margin-right") st.mar_r = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "margin-bottom") st.mar_b = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "margin-left") st.mar_l = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "border") {
                uint32_t c = 0u;
                float bw = 0.0f;
                css_detail::parse_border_shorthand(v, vw, vh, bw, c);
                st.bdr_t = st.bdr_r = st.bdr_b = st.bdr_l = bw;
                st.border_t_color = st.border_r_color = st.border_b_color = st.border_l_color = c;
            } else if (p == "border-top") {
                css_detail::parse_border_shorthand(v, vw, vh, st.bdr_t, st.border_t_color);
            } else if (p == "border-right") {
                css_detail::parse_border_shorthand(v, vw, vh, st.bdr_r, st.border_r_color);
            } else if (p == "border-bottom") {
                css_detail::parse_border_shorthand(v, vw, vh, st.bdr_b, st.border_b_color);
            } else if (p == "border-left") {
                css_detail::parse_border_shorthand(v, vw, vh, st.bdr_l, st.border_l_color);
            } else if (p == "border-color") {
                uint32_t c = css_detail::parse_color(v);
                st.border_t_color = st.border_r_color = st.border_b_color = st.border_l_color = c;
            } else if (p == "border-radius") {
                st.radius = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            } else if (p == "background" || p == "background-color") {
                st.bg_color = css_detail::parse_color(v);
            } else if (p == "color") {
                st.fg_color = css_detail::parse_color(v);
            } else if (p == "font-size") {
                st.font_px = std::clamp(static_cast<int>(std::round(css_detail::parse_length(v, vw, vh, false))), 9, 32);
            } else if (p == "font-weight") {
                st.font_bold = (v == "bold" || v == "600" || v == "700" || v == "800");
            } else if (p == "font-family") {
                std::string fl = detail::to_lower_ascii(v);
                st.font_mono = (fl.find("mono") != std::string::npos || fl.find("code") != std::string::npos || fl.find("consolas") != std::string::npos);
            } else if (p == "text-decoration") {
                st.text_underline = (v.find("underline") != std::string::npos);
            } else if (p == "white-space") {
                st.nowrap = (v == "nowrap" || v == "pre");
            } else if (p == "overflow" || p == "overflow-y") {
                if (v == "hidden") st.overflow = 1;
                else if (v == "auto" || v == "scroll") st.overflow = 2;
                else st.overflow = 0;
            } else if (p == "cursor") {
                st.cursor_pointer = (v == "pointer");
            } else if (p == "position") {
                st.position_mode = (v == "fixed" || v == "absolute") ? 1 : 0;
            } else if (p == "top") st.top = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "left") st.left = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "right") st.right = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "bottom") st.bottom = std::max(0.0f, css_detail::parse_length(v, vw, vh, false));
            else if (p == "box-shadow") {
                size_t rgba_pos = v.find("rgba(");
                if (rgba_pos != std::string::npos) {
                    st.shadow_color = css_detail::parse_color(v.substr(rgba_pos));
                    st.shadow_blur = 14.0f;
                }
            }
        }
    }

    for (auto& ch : node->children) {
        compute_styles_recursive(ch, sheet, vw, vh);
    }
}

namespace layout_detail {

inline float resolve_dim(float spec, float parent_dim, float fallback) {
    if (spec <= -1000.0f) {
        float pct = -(spec + 1000.0f);
        return parent_dim * (pct / 100.0f);
    }
    if (spec >= 0.0f) return spec;
    return fallback;
}

inline std::pair<float, float> intrinsic_size(const std::shared_ptr<DomNode>& n) {
    if (!n || n->style.display_none) return {0.0f, 0.0f};
    if (n->type == NodeType::Text) {
        auto p = n->parent.lock();
        return measure_text_run(n->text, p ? p->style : n->style);
    }
    const auto& st = n->style;
    float pad_x = st.pad_l + st.pad_r + st.bdr_l + st.bdr_r;
    float pad_y = st.pad_t + st.pad_b + st.bdr_t + st.bdr_b;

    if (n->tag == "input") {
        std::string txt = n->get_attr("value");
        if (txt.empty()) txt = n->get_attr("placeholder");
        auto [tw, th] = measure_text_run(txt.empty() ? "Search" : txt, st);
        float w = (st.width >= 0.0f) ? st.width : std::max(120.0f, tw + pad_x);
        float h = (st.height >= 0.0f) ? st.height : (th + pad_y);
        return {w, h};
    }

    if (n->attrs.count("data-icon")) {
        float sz = (st.width >= 0.0f) ? st.width : 15.0f;
        float h = (st.height >= 0.0f) ? st.height : sz;
        return {sz, h};
    }

    float cw = 0.0f, ch = 0.0f;
    bool horiz = (st.display_flex && !st.flex_column) || n->tag == "span" || n->tag == "button" || n->tag == "a";
    int vis_count = 0;
    for (const auto& c : n->children) {
        if (c->style.display_none || c->style.position_mode == 1) continue;
        auto [iw, ih] = intrinsic_size(c);
        iw += c->style.mar_l + c->style.mar_r;
        ih += c->style.mar_t + c->style.mar_b;
        if (horiz) {
            cw += iw;
            ch = std::max(ch, ih);
        } else {
            cw = std::max(cw, iw);
            ch += ih;
        }
        ++vis_count;
    }
    if (vis_count > 1) {
        if (horiz) cw += st.gap * static_cast<float>(vis_count - 1);
        else ch += st.gap * static_cast<float>(vis_count - 1);
    }
    float total_w = (st.width >= 0.0f) ? st.width : (cw + pad_x);
    float total_h = (st.height >= 0.0f) ? st.height : ((st.overflow != 0) ? pad_y : (ch + pad_y));
    total_w = std::clamp(total_w, st.min_width, st.max_width);
    total_h = std::clamp(total_h, st.min_height, st.max_height);
    return {total_w, total_h};
}

inline void layout_node(const std::shared_ptr<DomNode>& node, float x, float y, float avail_w, float avail_h, float vw, float vh) {
    if (!node || node->style.display_none) return;
    if (node->type == NodeType::Text) {
        auto p = node->parent.lock();
        auto [tw, th] = measure_text_run(node->text, p ? p->style : node->style);
        node->x = x;
        node->y = y;
        node->w = tw;
        node->h = th;
        node->content_h = th;
        return;
    }

    const auto& st = node->style;
    node->x = x;
    node->y = y;

    auto [intr_w, intr_h] = intrinsic_size(node);
    bool inline_like = (node->tag == "span" || node->tag == "button" || node->tag == "a" || node->attrs.count("data-icon") > 0);
    float def_w = inline_like ? intr_w : avail_w;
    node->w = std::clamp(resolve_dim(st.width, avail_w, def_w), st.min_width, st.max_width);
    node->h = std::clamp(resolve_dim(st.height, avail_h, intr_h), st.min_height, st.max_height);

    float inner_x = node->x + st.pad_l + st.bdr_l;
    float inner_y = node->y + st.pad_t + st.bdr_t;
    float inner_w = std::max(0.0f, node->w - (st.pad_l + st.pad_r + st.bdr_l + st.bdr_r));
    float inner_h = std::max(0.0f, node->h - (st.pad_t + st.pad_b + st.bdr_t + st.bdr_b));

    std::vector<std::shared_ptr<DomNode>> flow_children;
    std::vector<std::shared_ptr<DomNode>> overlay_children;
    for (auto& c : node->children) {
        if (c->style.display_none) continue;
        if (c->style.position_mode == 1) overlay_children.push_back(c);
        else flow_children.push_back(c);
    }

    bool all_inline = !flow_children.empty();
    for (const auto& c : flow_children) {
        if (c->type == NodeType::Element && c->tag != "span" && !c->attrs.count("data-icon")) {
            all_inline = false;
            break;
        }
    }

    if (st.display_flex || all_inline) {
        bool row = st.display_flex ? !st.flex_column : true;
        const size_t cnt = flow_children.size();
        std::vector<float> base(cnt, 0.0f);
        std::vector<float> cross_intr(cnt, 0.0f);
        float sum_base = 0.0f;
        float sum_grow = 0.0f;
        float max_cross = 0.0f;

        for (size_t i = 0; i < cnt; ++i) {
            const auto& c = flow_children[i];
            auto [cw, ch] = intrinsic_size(c);
            const auto& cs = c->style;
            if (row) {
                float spec_main = (cs.flex_basis >= 0.0f) ? cs.flex_basis : resolve_dim(cs.width, inner_w, cw);
                spec_main = std::clamp(spec_main, cs.min_width, cs.max_width);
                base[i] = spec_main;
                cross_intr[i] = ch;
                sum_base += spec_main + cs.mar_l + cs.mar_r;
                max_cross = std::max(max_cross, ch + cs.mar_t + cs.mar_b);
            } else {
                float spec_main = (cs.flex_basis >= 0.0f) ? cs.flex_basis : resolve_dim(cs.height, inner_h, ch);
                spec_main = std::clamp(spec_main, cs.min_height, cs.max_height);
                base[i] = spec_main;
                cross_intr[i] = cw;
                sum_base += spec_main + cs.mar_t + cs.mar_b;
                max_cross = std::max(max_cross, cw + cs.mar_l + cs.mar_r);
            }
            sum_grow += cs.flex_grow;
        }
        float total_gaps = cnt > 1 ? st.gap * static_cast<float>(cnt - 1) : 0.0f;
        float container_main = row ? inner_w : inner_h;
        float free_space = container_main - sum_base - total_gaps;

        if (free_space > 0.0f && sum_grow > 0.0f) {
            for (size_t i = 0; i < cnt; ++i) {
                if (flow_children[i]->style.flex_grow > 0.0f) {
                    base[i] += free_space * (flow_children[i]->style.flex_grow / sum_grow);
                }
            }
            free_space = 0.0f;
        } else if (free_space < 0.0f && st.overflow == 0) {
            float sum_shrink = 0.0f;
            for (size_t i = 0; i < cnt; ++i) sum_shrink += flow_children[i]->style.flex_shrink;
            if (sum_shrink > 0.0f) {
                for (size_t i = 0; i < cnt; ++i) {
                    float min_d = row ? flow_children[i]->style.min_width : flow_children[i]->style.min_height;
                    base[i] = std::max(min_d, base[i] + free_space * (flow_children[i]->style.flex_shrink / sum_shrink));
                }
            }
        }

        float cursor = 0.0f;
        float extra_gap = 0.0f;
        if (free_space > 0.0f && sum_grow == 0.0f) {
            if (st.justify == 1) cursor = free_space * 0.5f;
            else if (st.justify == 2) cursor = free_space;
            else if (st.justify == 3 && cnt > 1) extra_gap = free_space / static_cast<float>(cnt - 1);
        }

        if (row && st.height < 0.0f && st.height > -1000.0f) {
            inner_h = std::max(inner_h, max_cross);
            node->h = std::clamp(inner_h + st.pad_t + st.pad_b + st.bdr_t + st.bdr_b, st.min_height, st.max_height);
        }

        for (size_t i = 0; i < cnt; ++i) {
            auto& c = flow_children[i];
            const auto& cs = c->style;
            if (row) {
                float ch_h = (st.align == 0 && cs.height == -2.0f && c->type == NodeType::Element && c->tag != "span" && !c->attrs.count("data-icon"))
                                 ? std::max(cross_intr[i], inner_h - cs.mar_t - cs.mar_b)
                                 : resolve_dim(cs.height, inner_h, cross_intr[i]);
                float cy = inner_y + cs.mar_t;
                if (st.align == 2) cy = inner_y + (inner_h - ch_h) * 0.5f;
                else if (st.align == 3) cy = inner_y + inner_h - ch_h - cs.mar_b;

                float saved_w = cs.width;
                float saved_h = cs.height;
                c->style.width = base[i];
                if (ch_h > 0.0f) c->style.height = ch_h;
                layout_node(c, inner_x + cursor + cs.mar_l, cy, base[i], ch_h, vw, vh);
                c->style.width = saved_w;
                c->style.height = saved_h;
                cursor += cs.mar_l + c->w + cs.mar_r + st.gap + extra_gap;
            } else {
                float ch_w = (st.align == 0 && cs.width == -2.0f)
                                 ? std::max(0.0f, inner_w - cs.mar_l - cs.mar_r)
                                 : resolve_dim(cs.width, inner_w, cross_intr[i]);
                float cx = inner_x + cs.mar_l;
                if (st.align == 2) cx = inner_x + (inner_w - ch_w) * 0.5f;
                else if (st.align == 3) cx = inner_x + inner_w - ch_w - cs.mar_r;

                float saved_w = cs.width;
                float saved_h = cs.height;
                c->style.width = ch_w;
                if (cs.flex_grow > 0.0f || cs.height >= 0.0f || cs.height <= -1000.0f) {
                    c->style.height = base[i];
                }
                layout_node(c, cx, inner_y + cursor + cs.mar_t, ch_w, base[i], vw, vh);
                c->style.width = saved_w;
                c->style.height = saved_h;
                cursor += cs.mar_t + c->h + cs.mar_b + st.gap + extra_gap;
            }
        }
        node->content_h = row ? max_cross : std::max(0.0f, cursor - (cnt > 0 ? st.gap : 0.0f));
        if (!row && st.height == -2.0f) {
            node->h = std::clamp(node->content_h + st.pad_t + st.pad_b + st.bdr_t + st.bdr_b, st.min_height, st.max_height);
        }
    } else {
        // Block flow layout
        float cy = 0.0f;
        for (size_t i = 0; i < flow_children.size(); ++i) {
            auto& c = flow_children[i];
            const auto& cs = c->style;
            float cw = std::max(0.0f, inner_w - cs.mar_l - cs.mar_r);
            layout_node(c, inner_x + cs.mar_l, inner_y + cy + cs.mar_t, cw, inner_h, vw, vh);
            cy += cs.mar_t + c->h + cs.mar_b + (i + 1 < flow_children.size() ? st.gap : 0.0f);
        }
        node->content_h = cy;
        if (st.height == -2.0f && !node->attrs.count("data-icon") && node->tag != "input") {
            node->h = std::clamp(cy + st.pad_t + st.pad_b + st.bdr_t + st.bdr_b, st.min_height, st.max_height);
        }
    }

    node->scroll_y = std::clamp(node->scroll_y, 0.0f, std::max(0.0f, node->content_h - inner_h));

    for (auto& ov : overlay_children) {
        const auto& os = ov->style;
        auto [ow_i, oh_i] = intrinsic_size(ov);
        float ow = resolve_dim(os.width, vw, ow_i);
        float oh = resolve_dim(os.height, vh, oh_i);
        if (os.left >= 0.0f && os.right >= 0.0f && os.width == -2.0f) ow = vw - os.left - os.right;
        if (os.top >= 0.0f && os.bottom >= 0.0f && os.height == -2.0f) oh = vh - os.top - os.bottom;
        float ox = (os.left >= 0.0f) ? os.left : (os.right >= 0.0f ? vw - os.right - ow : (vw - ow) * 0.5f);
        float oy = (os.top >= 0.0f) ? os.top : (os.bottom >= 0.0f ? vh - os.bottom - oh : (vh - oh) * 0.5f);
        layout_node(ov, ox, oy, ow, oh, vw, vh);
    }
}

} // namespace layout_detail

inline void relayout_document(const std::shared_ptr<DomNode>& doc_root, const Stylesheet& sheet, float vw, float vh) {
    if (!doc_root) return;
    compute_styles_recursive(doc_root, sheet, vw, vh);
    doc_root->x = 0.0f;
    doc_root->y = 0.0f;
    doc_root->w = vw;
    doc_root->h = vh;
    for (auto& c : doc_root->children) {
        if (c->tag == "html" || c->tag == "body") {
            c->style.width = vw;
            c->style.height = vh;
        }
        layout_detail::layout_node(c, 0.0f, 0.0f, vw, vh, vw, vh);
    }
}

} // namespace helios
