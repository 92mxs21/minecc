#pragma once
// HeliosUI HTML5 Tokenizer & Live Document Object Model (clean-room C++20 implementation)

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace helios {

enum class NodeType : uint8_t {
    Document = 0,
    Element = 1,
    Text = 2,
};

struct ComputedStyle {
    bool display_none = false;
    bool display_flex = false;
    bool flex_column = false;
    int position_mode = 0; // 0 = static, 1 = fixed/absolute overlay
    float top = -1.0f, left = -1.0f, right = -1.0f, bottom = -1.0f;

    float flex_grow = 0.0f;
    float flex_shrink = 1.0f;
    float flex_basis = -1.0f; // <0 means auto

    // Lengths: >=0 px, -2 auto, <= -1000 means percentage (-1000 - pct)
    float width = -2.0f;
    float height = -2.0f;
    float min_width = 0.0f;
    float max_width = 1e9f;
    float min_height = 0.0f;
    float max_height = 1e9f;

    float pad_t = 0.0f, pad_r = 0.0f, pad_b = 0.0f, pad_l = 0.0f;
    float mar_t = 0.0f, mar_r = 0.0f, mar_b = 0.0f, mar_l = 0.0f;
    float bdr_t = 0.0f, bdr_r = 0.0f, bdr_b = 0.0f, bdr_l = 0.0f;
    float gap = 0.0f;
    float radius = 0.0f;

    int justify = 0; // 0 start, 1 center, 2 end, 3 space-between
    int align = 0;   // 0 stretch, 1 start, 2 center, 3 end
    int overflow = 0; // 0 visible, 1 hidden, 2 auto/scroll

    uint32_t bg_color = 0x00000000u; // ARGB
    uint32_t fg_color = 0xFFE6EDF3u; // ARGB
    uint32_t border_t_color = 0x00000000u;
    uint32_t border_r_color = 0x00000000u;
    uint32_t border_b_color = 0x00000000u;
    uint32_t border_l_color = 0x00000000u;
    uint32_t shadow_color = 0x00000000u;
    float shadow_blur = 0.0f;

    bool font_mono = false;
    bool font_bold = false;
    bool text_underline = false;
    bool nowrap = false;
    bool cursor_pointer = false;
    int font_px = 13;
};

struct DomNode : public std::enable_shared_from_this<DomNode> {
    NodeType type = NodeType::Element;
    uint32_t uid = 0;
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attrs;
    std::string text; // used when type == NodeType::Text, or input value

    std::weak_ptr<DomNode> parent;
    std::vector<std::shared_ptr<DomNode>> children;

    // Runtime interaction & scroll state
    float scroll_y = 0.0f;
    float scroll_x = 0.0f;
    bool hovered = false;
    bool active = false;
    bool focused = false;

    // Computed CSS & box geometry
    std::unordered_map<std::string, std::string> css_vars;
    ComputedStyle style;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float content_h = 0.0f;

    bool has_class(std::string_view cls) const {
        for (const auto& c : classes) {
            if (c == cls) return true;
        }
        return false;
    }

    void add_class(const std::string& cls) {
        if (!cls.empty() && !has_class(cls)) classes.push_back(cls);
    }

    void remove_class(std::string_view cls) {
        classes.erase(
            std::remove_if(classes.begin(), classes.end(), [&](const std::string& c) { return c == cls; }),
            classes.end());
    }

    void sync_classes_from_attr() {
        classes.clear();
        auto it = attrs.find("class");
        if (it == attrs.end()) return;
        const std::string& s = it->second;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t start = i;
            while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i > start) classes.push_back(s.substr(start, i - start));
        }
    }

    void set_attr(const std::string& k, const std::string& v) {
        attrs[k] = v;
        if (k == "id") id = v;
        else if (k == "class") sync_classes_from_attr();
    }

    std::string get_attr(const std::string& k) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? "" : it->second;
    }

    std::string text_content() const {
        if (type == NodeType::Text) return text;
        if (tag == "input") return get_attr("value");
        std::string out;
        for (const auto& ch : children) out += ch->text_content();
        return out;
    }

    void set_text_content(const std::string& val, uint32_t& next_uid) {
        if (tag == "input") {
            set_attr("value", val);
            return;
        }
        children.clear();
        if (!val.empty()) {
            auto t = std::make_shared<DomNode>();
            t->type = NodeType::Text;
            t->uid = ++next_uid;
            t->text = val;
            t->parent = weak_from_this();
            children.push_back(std::move(t));
        }
    }

    std::shared_ptr<DomNode> get_element_by_id(std::string_view target_id) {
        if (type == NodeType::Element && id == target_id) return shared_from_this();
        for (auto& c : children) {
            if (auto found = c->get_element_by_id(target_id)) return found;
        }
        return nullptr;
    }
};

namespace detail {

inline std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

inline bool is_void_html_element(std::string_view tag) {
    return tag == "area" || tag == "base" || tag == "br" || tag == "col" ||
           tag == "embed" || tag == "hr" || tag == "img" || tag == "input" ||
           tag == "link" || tag == "meta" || tag == "source" || tag == "track" || tag == "wbr";
}

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

inline std::string decode_entities(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '&') {
            size_t semi = src.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 12) {
                std::string_view ent = src.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out.push_back('&'); i = semi + 1; continue; }
                if (ent == "lt") { out.push_back('<'); i = semi + 1; continue; }
                if (ent == "gt") { out.push_back('>'); i = semi + 1; continue; }
                if (ent == "quot") { out.push_back('"'); i = semi + 1; continue; }
                if (ent == "apos" || ent == "#39") { out.push_back('\''); i = semi + 1; continue; }
                if (ent == "nbsp") { out.push_back(' '); i = semi + 1; continue; }
                if (ent == "middot") { append_utf8(out, 0x00B7u); i = semi + 1; continue; }
                if (ent == "times") { append_utf8(out, 0x00D7u); i = semi + 1; continue; }
                if (ent == "rarr") { append_utf8(out, 0x2192u); i = semi + 1; continue; }
                if (ent == "larr") { append_utf8(out, 0x2190u); i = semi + 1; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    uint32_t cp = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        cp = static_cast<uint32_t>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16));
                    } else {
                        cp = static_cast<uint32_t>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10));
                    }
                    if (cp > 0) {
                        append_utf8(out, cp);
                        i = semi + 1;
                        continue;
                    }
                }
            }
        }
        out.push_back(src[i++]);
    }
    return out;
}

} // namespace detail

struct ParsedHtmlDocument {
    std::shared_ptr<DomNode> root;
    std::vector<std::string> inline_styles;
    std::vector<std::string> linked_stylesheets;
    std::vector<std::string> inline_scripts;
    std::vector<std::string> linked_scripts;
    uint32_t last_uid = 0;
};

inline void parse_html_into(
    std::string_view html,
    const std::shared_ptr<DomNode>& container,
    uint32_t& uid_counter,
    std::vector<std::string>* out_styles = nullptr,
    std::vector<std::string>* out_linked_css = nullptr,
    std::vector<std::string>* out_scripts = nullptr,
    std::vector<std::string>* out_linked_js = nullptr,
    bool preserve_whitespace = false)
{
    std::vector<std::shared_ptr<DomNode>> stack = {container};
    size_t i = 0;
    const size_t n = html.size();

    auto flush_text = [&](std::string_view raw_txt) {
        if (raw_txt.empty()) return;
        const std::string& parent_tag = stack.back()->tag;
        bool pre_ctx = preserve_whitespace || parent_tag == "pre" || parent_tag == "code" || parent_tag == "span";
        std::string decoded = detail::decode_entities(raw_txt);
        if (!pre_ctx) {
            // Collapse runs of whitespace per HTML5 inline white-space rules; drop pure-indent gaps
            std::string collapsed;
            collapsed.reserve(decoded.size());
            bool prev_space = false;
            bool any_non_space = false;
            for (unsigned char ch : decoded) {
                if (std::isspace(ch)) {
                    if (!prev_space) {
                        collapsed.push_back(' ');
                        prev_space = true;
                    }
                } else {
                    collapsed.push_back(static_cast<char>(ch));
                    prev_space = false;
                    any_non_space = true;
                }
            }
            if (!any_non_space) {
                // Keep a single separating space only inside inline spans/code lines if single space
                if (decoded == " ") collapsed = " ";
                else return;
            }
            decoded = std::move(collapsed);
        }
        if (decoded.empty()) return;
        auto node = std::make_shared<DomNode>();
        node->type = NodeType::Text;
        node->uid = ++uid_counter;
        node->text = std::move(decoded);
        node->parent = stack.back();
        stack.back()->children.push_back(std::move(node));
    };

    while (i < n) {
        size_t lt = html.find('<', i);
        if (lt == std::string_view::npos) {
            flush_text(html.substr(i));
            break;
        }
        if (lt > i) flush_text(html.substr(i, lt - i));

        // HTML comment <!-- ... -->
        if (html.substr(lt, 4) == "<!--") {
            size_t end_c = html.find("-->", lt + 4);
            if (end_c == std::string_view::npos) break;
            i = end_c + 3;
            continue;
        }
        // DOCTYPE / declarations <!DOCTYPE html>
        if (lt + 1 < n && html[lt + 1] == '!') {
            size_t gt = html.find('>', lt + 2);
            if (gt == std::string_view::npos) break;
            i = gt + 1;
            continue;
        }
        // Closing tag </tag>
        if (lt + 1 < n && html[lt + 1] == '/') {
            size_t gt = html.find('>', lt + 2);
            if (gt == std::string_view::npos) break;
            std::string close_name = detail::to_lower_ascii(html.substr(lt + 2, gt - lt - 2));
            while (!close_name.empty() && std::isspace(static_cast<unsigned char>(close_name.back()))) {
                close_name.pop_back();
            }
            for (size_t s = stack.size(); s > 1; --s) {
                if (stack[s - 1]->tag == close_name) {
                    stack.resize(s - 1);
                    break;
                }
            }
            i = gt + 1;
            continue;
        }

        // Opening tag <tag attr="val" ...>
        size_t cur = lt + 1;
        while (cur < n && std::isspace(static_cast<unsigned char>(html[cur]))) ++cur;
        size_t name_start = cur;
        while (cur < n && !std::isspace(static_cast<unsigned char>(html[cur])) &&
               html[cur] != '>' && html[cur] != '/')
        {
            ++cur;
        }
        std::string tag_name = detail::to_lower_ascii(html.substr(name_start, cur - name_start));
        if (tag_name.empty()) {
            i = lt + 1;
            continue;
        }

        auto elem = std::make_shared<DomNode>();
        elem->type = NodeType::Element;
        elem->uid = ++uid_counter;
        elem->tag = tag_name;

        bool self_closing = false;
        while (cur < n) {
            while (cur < n && std::isspace(static_cast<unsigned char>(html[cur]))) ++cur;
            if (cur >= n) break;
            if (html[cur] == '>') {
                ++cur;
                break;
            }
            if (html[cur] == '/' && cur + 1 < n && html[cur + 1] == '>') {
                self_closing = true;
                cur += 2;
                break;
            }
            size_t a_start = cur;
            while (cur < n && !std::isspace(static_cast<unsigned char>(html[cur])) &&
                   html[cur] != '=' && html[cur] != '>' && html[cur] != '/')
            {
                ++cur;
            }
            std::string attr_name = detail::to_lower_ascii(html.substr(a_start, cur - a_start));
            while (cur < n && std::isspace(static_cast<unsigned char>(html[cur]))) ++cur;
            std::string attr_val;
            if (cur < n && html[cur] == '=') {
                ++cur;
                while (cur < n && std::isspace(static_cast<unsigned char>(html[cur]))) ++cur;
                if (cur < n && (html[cur] == '"' || html[cur] == '\'')) {
                    char q = html[cur++];
                    size_t v_start = cur;
                    while (cur < n && html[cur] != q) ++cur;
                    attr_val = detail::decode_entities(html.substr(v_start, cur - v_start));
                    if (cur < n && html[cur] == q) ++cur;
                } else {
                    size_t v_start = cur;
                    while (cur < n && !std::isspace(static_cast<unsigned char>(html[cur])) && html[cur] != '>') {
                        ++cur;
                    }
                    attr_val = detail::decode_entities(html.substr(v_start, cur - v_start));
                }
            }
            if (!attr_name.empty()) elem->set_attr(attr_name, attr_val);
        }
        i = cur;

        // Raw-text elements per HTML5 spec (<style> and <script>)
        if (tag_name == "style") {
            size_t end_pos = i;
            while (end_pos < n) {
                size_t close_lt = html.find("</", end_pos);
                if (close_lt == std::string_view::npos) { end_pos = n; break; }
                if (detail::to_lower_ascii(html.substr(close_lt + 2, 5)) == "style") {
                    if (out_styles) out_styles->emplace_back(html.substr(i, close_lt - i));
                    size_t gt = html.find('>', close_lt + 7);
                    i = (gt == std::string_view::npos) ? n : gt + 1;
                    break;
                }
                end_pos = close_lt + 2;
            }
            continue;
        }
        if (tag_name == "script") {
            std::string src_attr = elem->get_attr("src");
            if (!src_attr.empty() && out_linked_js) out_linked_js->push_back(src_attr);
            if (!self_closing) {
                size_t end_pos = i;
                while (end_pos < n) {
                    size_t close_lt = html.find("</", end_pos);
                    if (close_lt == std::string_view::npos) { i = n; break; }
                    if (detail::to_lower_ascii(html.substr(close_lt + 2, 6)) == "script") {
                        std::string body(html.substr(i, close_lt - i));
                        if (src_attr.empty() && out_scripts) out_scripts->push_back(std::move(body));
                        size_t gt = html.find('>', close_lt + 8);
                        i = (gt == std::string_view::npos) ? n : gt + 1;
                        break;
                    }
                    end_pos = close_lt + 2;
                }
            }
            continue;
        }
        if (tag_name == "link" && detail::to_lower_ascii(elem->get_attr("rel")) == "stylesheet") {
            std::string href = elem->get_attr("href");
            if (!href.empty() && out_linked_css) out_linked_css->push_back(href);
            continue;
        }
        if (tag_name == "title") {
            size_t close_t = html.find("</", i);
            size_t gt = (close_t == std::string_view::npos) ? std::string_view::npos : html.find('>', close_t + 2);
            i = (gt == std::string_view::npos) ? n : gt + 1;
            continue;
        }
        if (tag_name == "head" || tag_name == "meta") {
            continue;
        }

        elem->parent = stack.back();
        stack.back()->children.push_back(elem);
        if (!self_closing && !detail::is_void_html_element(tag_name)) {
            stack.push_back(elem);
        }
    }
}

inline ParsedHtmlDocument parse_html_document(std::string_view html) {
    ParsedHtmlDocument doc;
    doc.root = std::make_shared<DomNode>();
    doc.root->type = NodeType::Document;
    doc.root->tag = "#document";
    doc.last_uid = 1;
    parse_html_into(
        html,
        doc.root,
        doc.last_uid,
        &doc.inline_styles,
        &doc.linked_stylesheets,
        &doc.inline_scripts,
        &doc.linked_scripts);
    return doc;
}

} // namespace helios
