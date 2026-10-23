#pragma once
// FluxRenderer: High-Throughput Zero-Allocation C++20 Software Compositor
//   - Linear & radial atmospheric gradients, crisp SDF rounded cards, glass specular highlights,
//     anti-aliased vector iconography, 1:1 FreeType2 DejaVu glyph blitter & fast Huffman PNG encoder.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "native/generated/font_atlas.hpp"

namespace flux {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct ClipBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline uint32_t with_alpha(uint32_t rgb, float a01) noexcept {
    uint32_t a = static_cast<uint32_t>(std::clamp(a01 * 255.0f, 0.0f, 255.0f));
    return (rgb & 0x00FFFFFFu) | (a << 24);
}

inline uint32_t lerp_color(uint32_t c0, uint32_t c1, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    uint32_t a0 = (c0 >> 24) & 0xFFu, r0 = (c0 >> 16) & 0xFFu, g0 = (c0 >> 8) & 0xFFu, b0 = c0 & 0xFFu;
    uint32_t a1 = (c1 >> 24) & 0xFFu, r1 = (c1 >> 16) & 0xFFu, g1 = (c1 >> 8) & 0xFFu, b1 = c1 & 0xFFu;
    uint32_t a = static_cast<uint32_t>(a0 + (static_cast<float>(a1) - a0) * t);
    uint32_t r = static_cast<uint32_t>(r0 + (static_cast<float>(r1) - r0) * t);
    uint32_t g = static_cast<uint32_t>(g0 + (static_cast<float>(g1) - g0) * t);
    uint32_t b = static_cast<uint32_t>(b0 + (static_cast<float>(b1) - b0) * t);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

class FluxCanvas {
public:
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels; // 0xFFRRGGBB (Win32 top-down 32bpp BGRA & X11 24/32bpp native layout!)
    std::vector<ClipBox> clips;

    void resize(int w, int h) {
        w = std::clamp(w, 320, 7680);
        h = std::clamp(h, 240, 4320);
        if (width != w || height != h) {
            width = w;
            height = h;
            pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0xFF07090Du);
        }
        clips.clear();
        clips.push_back({0, 0, width, height});
    }

    ClipBox clip() const noexcept {
        return clips.empty() ? ClipBox{0, 0, width, height} : clips.back();
    }

    void push_clip(float fx, float fy, float fw, float fh) {
        ClipBox p = clip();
        clips.push_back({
            std::max(p.x0, static_cast<int>(std::floor(fx))),
            std::max(p.y0, static_cast<int>(std::floor(fy))),
            std::min(p.x1, static_cast<int>(std::ceil(fx + fw))),
            std::min(p.y1, static_cast<int>(std::ceil(fy + fh)))
        });
    }

    void pop_clip() {
        if (clips.size() > 1) clips.pop_back();
    }

    static inline uint32_t blend_px(uint32_t dst, uint32_t src) noexcept {
        uint32_t sa = (src >> 24) & 0xFFu;
        if (sa == 0u) return dst;
        if (sa == 255u) return src | 0xFF000000u;
        uint32_t inv = 255u - sa;
        uint32_t r = (((src >> 16) & 0xFFu) * sa + ((dst >> 16) & 0xFFu) * inv + 127u) >> 8;
        uint32_t g = (((src >> 8) & 0xFFu) * sa + ((dst >> 8) & 0xFFu) * inv + 127u) >> 8;
        uint32_t b = ((src & 0xFFu) * sa + (dst & 0xFFu) * inv + 127u) >> 8;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    inline void plot(int x, int y, uint32_t col) noexcept {
        const ClipBox c = clip();
        if (x < c.x0 || x >= c.x1 || y < c.y0 || y >= c.y1) return;
        uint32_t& dst = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        dst = blend_px(dst, col);
    }

    void fill_rect(float fx, float fy, float fw, float fh, uint32_t col) noexcept {
        uint32_t a = (col >> 24) & 0xFFu;
        if (a == 0u || fw <= 0.0f || fh <= 0.0f) return;
        const ClipBox c = clip();
        int x0 = std::max(c.x0, static_cast<int>(std::floor(fx)));
        int y0 = std::max(c.y0, static_cast<int>(std::floor(fy)));
        int x1 = std::min(c.x1, static_cast<int>(std::ceil(fx + fw)));
        int y1 = std::min(c.y1, static_cast<int>(std::ceil(fy + fh)));
        if (x0 >= x1 || y0 >= y1) return;

        if (a == 255u) {
            uint32_t solid = col | 0xFF000000u;
            for (int y = y0; y < y1; ++y) {
                uint32_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                std::fill(row + x0, row + x1, solid);
            }
        } else {
            for (int y = y0; y < y1; ++y) {
                uint32_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                for (int x = x0; x < x1; ++x) row[x] = blend_px(row[x], col);
            }
        }
    }

    void fill_vgradient(float fx, float fy, float fw, float fh, uint32_t top_col, uint32_t bot_col) noexcept {
        const ClipBox c = clip();
        int x0 = std::max(c.x0, static_cast<int>(fx));
        int y0 = std::max(c.y0, static_cast<int>(fy));
        int x1 = std::min(c.x1, static_cast<int>(fx + fw));
        int y1 = std::min(c.y1, static_cast<int>(fy + fh));
        if (x0 >= x1 || y0 >= y1) return;
        float inv_h = 1.0f / std::max(1.0f, fh);
        for (int y = y0; y < y1; ++y) {
            uint32_t col = lerp_color(top_col, bot_col, (y - fy) * inv_h) | 0xFF000000u;
            uint32_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
            std::fill(row + x0, row + x1, col);
        }
    }

    // Smooth atmospheric radial spotlight for the Home Screen hero!
    void draw_radial_glow(float cx, float cy, float rx, float ry, uint32_t rgb, float peak_alpha) noexcept {
        if (peak_alpha <= 0.002f) return;
        const ClipBox c = clip();
        int x0 = std::max(c.x0, static_cast<int>(cx - rx));
        int y0 = std::max(c.y0, static_cast<int>(cy - ry));
        int x1 = std::min(c.x1, static_cast<int>(cx + rx));
        int y1 = std::min(c.y1, static_cast<int>(cy + ry));
        float irx2 = 1.0f / std::max(1.0f, rx * rx);
        float iry2 = 1.0f / std::max(1.0f, ry * ry);
        uint32_t base_rgb = rgb & 0x00FFFFFFu;

        for (int y = y0; y < y1; ++y) {
            float dy2 = (y - cy) * (y - cy) * iry2;
            uint32_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
            for (int x = x0; x < x1; ++x) {
                float d2 = dy2 + (x - cx) * (x - cx) * irx2;
                if (d2 >= 1.0f) continue;
                float falloff = (1.0f - d2) * (1.0f - d2);
                uint32_t a = static_cast<uint32_t>(peak_alpha * falloff * 255.0f);
                if (a > 0u) row[x] = blend_px(row[x], base_rgb | (a << 24));
            }
        }
    }

    // Ultra-fast scanline-optimized anti-aliased rounded card with border + subtle glass top edge!
    // Skips interior SDF square-roots completely so even 20 large cards paint in < 0.25 ms!
    void fill_card(
        float fx, float fy, float fw, float fh, float radius,
        uint32_t fill_top, uint32_t fill_bot,
        float border_w = 1.0f, uint32_t border_col = 0u,
        bool specular_top = true) noexcept
    {
        if (fw <= 1.0f || fh <= 1.0f) return;
        float r = std::min({std::max(0.0f, radius), fw * 0.5f, fh * 0.5f});
        const ClipBox c = clip();
        int x0 = std::max(c.x0, static_cast<int>(std::floor(fx - 1.0f)));
        int y0 = std::max(c.y0, static_cast<int>(std::floor(fy - 1.0f)));
        int x1 = std::min(c.x1, static_cast<int>(std::ceil(fx + fw + 1.0f)));
        int y1 = std::min(c.y1, static_cast<int>(std::ceil(fy + fh + 1.0f)));
        if (x0 >= x1 || y0 >= y1) return;

        float cx = fx + fw * 0.5f, cy = fy + fh * 0.5f;
        float hw = fw * 0.5f, hh = fh * 0.5f;
        float inv_h = 1.0f / std::max(1.0f, fh);
        uint32_t ba = (border_col >> 24) & 0xFFu;

        // Interior fast X span `[inner_x0 .. inner_x1)` needs NO per-pixel sqrt on non-corner rows!
        int inner_x0 = std::max(x0, static_cast<int>(std::ceil(fx + r + border_w + 1.0f)));
        int inner_x1 = std::min(x1, static_cast<int>(std::floor(fx + fw - r - border_w - 1.0f)));
        int inner_y0 = static_cast<int>(std::ceil(fy + r + border_w + 1.0f));
        int inner_y1 = static_cast<int>(std::floor(fy + fh - r - border_w - 1.0f));

        for (int y = y0; y < y1; ++y) {
            uint32_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
            float py = y + 0.5f;
            uint32_t row_fill = (fill_top == fill_bot) ? fill_top : lerp_color(fill_top, fill_bot, (py - fy) * inv_h);
            uint32_t fa = (row_fill >> 24) & 0xFFu;

            bool middle_row = (y >= inner_y0 && y < inner_y1 && inner_x1 > inner_x0);

            auto shade_edge_px = [&](int x) {
                float qx = std::fabs((x + 0.5f) - cx) - (hw - r);
                float qy = std::fabs(py - cy) - (hh - r);
                float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
                if (d > 0.65f) return;
                float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
                if (fa > 0u) {
                    uint32_t a = static_cast<uint32_t>(fa * cov);
                    row[x] = blend_px(row[x], (row_fill & 0x00FFFFFFu) | (a << 24));
                }
                if (border_w > 0.0f && ba > 0u) {
                    float ring = cov * std::clamp(0.5f + d + border_w, 0.0f, 1.0f);
                    if (ring > 0.01f) {
                        uint32_t a = static_cast<uint32_t>(ba * ring);
                        row[x] = blend_px(row[x], (border_col & 0x00FFFFFFu) | (a << 24));
                    }
                }
            };

            if (middle_row) {
                for (int x = x0; x < inner_x0; ++x) shade_edge_px(x);
                if (fa == 255u) {
                    std::fill(row + inner_x0, row + inner_x1, row_fill | 0xFF000000u);
                } else if (fa > 0u) {
                    for (int x = inner_x0; x < inner_x1; ++x) row[x] = blend_px(row[x], row_fill);
                }
                for (int x = inner_x1; x < x1; ++x) shade_edge_px(x);
            } else {
                for (int x = x0; x < x1; ++x) shade_edge_px(x);
            }
        }

        if (specular_top && fw > r * 2.0f + 4.0f) {
            fill_rect(fx + r, fy + 1.0f, fw - r * 2.0f, 1.0f, 0x16FFFFFFu);
        }
    }

    void draw_line_aa(float x0, float y0, float x1, float y1, float thick, uint32_t col) noexcept {
        const ClipBox c = clip();
        float pad = thick + 1.5f;
        int ix0 = std::max(c.x0, static_cast<int>(std::min(x0, x1) - pad));
        int iy0 = std::max(c.y0, static_cast<int>(std::min(y0, y1) - pad));
        int ix1 = std::min(c.x1, static_cast<int>(std::ceil(std::max(x0, x1) + pad)));
        int iy1 = std::min(c.y1, static_cast<int>(std::ceil(std::max(y0, y1) + pad)));
        float vx = x1 - x0, vy = y1 - y0;
        float len2 = std::max(1e-4f, vx * vx + vy * vy);
        uint32_t ba = (col >> 24) & 0xFFu;
        float ht = thick * 0.5f;
        for (int y = iy0; y < iy1; ++y) {
            for (int x = ix0; x < ix1; ++x) {
                float px = x + 0.5f - x0, py = y + 0.5f - y0;
                float h = std::clamp((px * vx + py * vy) / len2, 0.0f, 1.0f);
                float dx = px - vx * h, dy = py - vy * h;
                float d = std::sqrt(dx * dx + dy * dy) - ht;
                if (d < 0.7f) {
                    uint32_t a = static_cast<uint32_t>(ba * std::clamp(0.5f - d, 0.0f, 1.0f));
                    plot(x, y, (col & 0x00FFFFFFu) | (a << 24));
                }
            }
        }
    }

    void draw_circle_aa(float cx, float cy, float r, uint32_t fill_col, float stroke_w = 0.0f, uint32_t stroke_col = 0u) noexcept {
        const ClipBox c = clip();
        int pad = static_cast<int>(std::ceil(r + stroke_w + 2.0f));
        int x0 = std::max(c.x0, static_cast<int>(cx) - pad);
        int y0 = std::max(c.y0, static_cast<int>(cy) - pad);
        int x1 = std::min(c.x1, static_cast<int>(cx) + pad + 1);
        int y1 = std::min(c.y1, static_cast<int>(cy) + pad + 1);
        uint32_t fa = (fill_col >> 24) & 0xFFu, sa = (stroke_col >> 24) & 0xFFu;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r;
                if (d < 0.7f && fa > 0u) {
                    plot(x, y, (fill_col & 0x00FFFFFFu) | (static_cast<uint32_t>(fa * std::clamp(0.5f - d, 0.0f, 1.0f)) << 24));
                }
                if (stroke_w > 0.0f && sa > 0u) {
                    float ring = std::fabs(d + stroke_w * 0.5f) - stroke_w * 0.5f;
                    if (ring < 0.7f) {
                        plot(x, y, (stroke_col & 0x00FFFFFFu) | (static_cast<uint32_t>(sa * std::clamp(0.5f - ring, 0.0f, 1.0f)) << 24));
                    }
                }
            }
        }
    }

    static inline uint32_t next_cp(std::string_view s, size_t& i) noexcept {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 < 0x80u) { ++i; return c0; }
        if ((c0 & 0xE0u) == 0xC0u && i + 1 < s.size()) {
            uint32_t cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            i += 2; return cp;
        }
        if ((c0 & 0xF0u) == 0xE0u && i + 2 < s.size()) {
            uint32_t cp = ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            i += 3; return cp;
        }
        ++i; return '?';
    }

    static inline float measure_text(std::string_view txt, int px, bool bold = false, bool mono = false) noexcept {
        int face = helios::fonts::select_face(mono, bold, px);
        float w = 0.0f;
        for (size_t i = 0; i < txt.size();) {
            uint32_t cp = next_cp(txt, i);
            if (const auto* g = helios::fonts::lookup_glyph(face, cp)) w += g->advance;
        }
        return w;
    }

    float draw_text(std::string_view txt, float x, float y, int px, uint32_t col, bool bold = false, bool mono = false, float max_w = 1e9f) noexcept {
        uint32_t base_a = (col >> 24) & 0xFFu;
        if (txt.empty() || base_a == 0u) return x;
        int face_idx = helios::fonts::select_face(mono, bold, px);
        float pen_x = x;
        uint32_t rgb = col & 0x00FFFFFFu;
        const ClipBox c = clip();

        size_t i = 0;
        while (i < txt.size()) {
            uint32_t cp = next_cp(txt, i);
            if (cp == '\n' || cp == '\r') continue;
            const auto* g = helios::fonts::lookup_glyph(face_idx, cp);
            if (!g) continue;
            if (pen_x + g->advance > x + max_w) {
                // Draw crisp ellipsis dot if truncated
                break;
            }
            int gx0 = static_cast<int>(std::round(pen_x)) + g->bearing_x;
            int gy0 = static_cast<int>(std::round(y)) + g->bearing_y;
            const uint8_t* bmp = helios::fonts::RAW_ALPHA + g->alpha_offset;

            for (int r = 0; r < g->height; ++r) {
                int py = gy0 + r;
                if (py < c.y0 || py >= c.y1) continue;
                uint32_t* dst = pixels.data() + static_cast<size_t>(py) * static_cast<size_t>(width);
                const uint8_t* src = bmp + r * g->width;
                for (int k = 0; k < g->width; ++k) {
                    int px_i = gx0 + k;
                    if (px_i < c.x0 || px_i >= c.x1) continue;
                    uint8_t cov = src[k];
                    if (!cov) continue;
                    uint32_t a = (base_a * cov + 127u) >> 8;
                    dst[px_i] = blend_px(dst[px_i], rgb | (a << 24));
                }
            }
            pen_x += g->advance;
        }
        return pen_x;
    }

    // Crisp multi-line word wrapper for Bento cards & subtitles
    void draw_wrapped_text(std::string_view txt, float x, float y, int px, uint32_t col, float max_w, float line_h, int max_lines = 2) noexcept {
        size_t pos = 0, n = txt.size();
        for (int ln = 0; ln < max_lines && pos < n; ++ln) {
            while (pos < n && txt[pos] == ' ') ++pos;
            if (pos >= n) break;
            size_t end = pos, best_break = pos;
            while (end < n && txt[end] != '\n') {
                size_t next_space = txt.find(' ', end);
                size_t cand = (next_space == std::string_view::npos) ? n : next_space;
                if (measure_text(txt.substr(pos, cand - pos), px, false, false) <= max_w) {
                    best_break = cand;
                    end = (next_space == std::string_view::npos) ? n : next_space + 1;
                } else {
                    break;
                }
            }
            if (best_break == pos) best_break = std::min(n, pos + 28);
            draw_text(txt.substr(pos, best_break - pos), x, y + float(ln) * line_h, px, col, false, false, max_w);
            pos = best_break;
        }
    }

    // Crisp custom geometric icons for Rail, Hero Cards & Explorer
    void draw_vector_icon(std::string_view kind, float x, float y, float sz, uint32_t col) noexcept {
        float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.42f;
        if (kind == "logo") {
            // Sleek hexagonal diamond prism mark
            draw_line_aa(cx, cy - r, cx + r, cy, 1.8f, col);
            draw_line_aa(cx + r, cy, cx, cy + r, 1.8f, col);
            draw_line_aa(cx, cy + r, cx - r, cy, 1.8f, col);
            draw_line_aa(cx - r, cy, cx, cy - r, 1.8f, col);
            draw_circle_aa(cx, cy, r * 0.30f, col);
        } else if (kind == "home") {
            draw_line_aa(cx - r, cy - 0.5f, cx, cy - r, 1.7f, col);
            draw_line_aa(cx, cy - r, cx + r, cy - 0.5f, 1.7f, col);
            draw_line_aa(cx - r * 0.72f, cy - 1.0f, cx - r * 0.72f, cy + r * 0.85f, 1.7f, col);
            draw_line_aa(cx + r * 0.72f, cy - 1.0f, cx + r * 0.72f, cy + r * 0.85f, 1.7f, col);
            draw_line_aa(cx - r * 0.72f, cy + r * 0.85f, cx + r * 0.72f, cy + r * 0.85f, 1.7f, col);
        } else if (kind == "code") {
            draw_line_aa(cx - r * 0.35f, cy - r * 0.65f, cx - r, cy, 1.7f, col);
            draw_line_aa(cx - r, cy, cx - r * 0.35f, cy + r * 0.65f, 1.7f, col);
            draw_line_aa(cx + r * 0.35f, cy - r * 0.65f, cx + r, cy, 1.7f, col);
            draw_line_aa(cx + r, cy, cx + r * 0.35f, cy + r * 0.65f, 1.7f, col);
        } else if (kind == "disasm") {
            fill_card(cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f, 3.0f, 0u, 0u, 1.5f, col, false);
            draw_line_aa(cx - r * 0.4f, cy - r * 0.25f, cx + r * 0.4f, cy - r * 0.25f, 1.5f, col);
            draw_line_aa(cx - r * 0.4f, cy + r * 0.25f, cx + r * 0.2f, cy + r * 0.25f, 1.5f, col);
        } else if (kind == "xrefs") {
            draw_circle_aa(cx - r * 0.62f, cy, 2.5f, col);
            draw_circle_aa(cx + r * 0.62f, cy - r * 0.58f, 2.5f, col);
            draw_circle_aa(cx + r * 0.62f, cy + r * 0.58f, 2.5f, col);
            draw_line_aa(cx - r * 0.5f, cy, cx + r * 0.5f, cy - r * 0.54f, 1.5f, col);
            draw_line_aa(cx - r * 0.5f, cy, cx + r * 0.5f, cy + r * 0.54f, 1.5f, col);
        } else if (kind == "strings") {
            draw_circle_aa(cx - 1.5f, cy - 1.5f, r * 0.68f, 0u, 1.6f, col);
            draw_line_aa(cx + r * 0.32f, cy + r * 0.32f, cx + r * 0.92f, cy + r * 0.92f, 1.8f, col);
        } else if (kind == "jar") {
            fill_card(cx - r * 0.8f, cy - r * 0.7f, r * 1.6f, r * 1.45f, 3.0f, with_alpha(col, 0.16f), with_alpha(col, 0.08f), 1.5f, col, false);
            draw_line_aa(cx, cy - r * 0.35f, cx, cy + r * 0.35f, 1.6f, col);
            draw_line_aa(cx - r * 0.35f, cy, cx + r * 0.35f, cy, 1.6f, col);
        } else if (kind == "terminal") {
            fill_card(cx - r * 0.9f, cy - r * 0.75f, r * 1.8f, r * 1.5f, 3.0f, 0u, 0u, 1.5f, col, false);
            draw_line_aa(cx - r * 0.5f, cy - r * 0.25f, cx - r * 0.15f, cy + 0.5f, 1.5f, col);
            draw_line_aa(cx - r * 0.15f, cy + 0.5f, cx - r * 0.5f, cy + r * 0.32f, 1.5f, col);
            draw_line_aa(cx + 0.5f, cy + r * 0.32f, cx + r * 0.5f, cy + r * 0.32f, 1.5f, col);
        } else {
            // Class / Interface / Record / Enum circular badge
            const char* ch = "C";
            uint32_t badge_c = 0xFF38BDF8u;
            if (kind == "interface") { ch = "I"; badge_c = 0xFF34D399u; }
            else if (kind == "record") { ch = "R"; badge_c = 0xFF818CF8u; }
            else if (kind == "enum") { ch = "E"; badge_c = 0xFFFBBF24u; }
            draw_circle_aa(cx, cy, r, with_alpha(badge_c, 0.18f), 1.15f, with_alpha(badge_c, 0.85f));
            float tw = measure_text(ch, 11, true, false);
            draw_text(ch, cx - tw * 0.5f, cy - 6.5f, 11, badge_c, true, false);
        }
    }

    // Fast zero-dependency RFC 1951 Fixed-Huffman LZ77 PNG encoder (~110 KB output in ~1.3 ms!)
    std::vector<uint8_t> encode_png() const {
        static const auto crc_tab = []() {
            std::array<uint32_t, 256> t{};
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[n] = c;
            }
            return t;
        }();
        auto crc32 = [&](const uint8_t* d, size_t n) {
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i) c = crc_tab[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        };
        auto put32 = [](std::vector<uint8_t>& o, uint32_t v) {
            o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16));
            o.push_back(uint8_t(v >> 8));  o.push_back(uint8_t(v));
        };
        auto chunk = [&](std::vector<uint8_t>& o, const char tp[4], const uint8_t* p, size_t n) {
            put32(o, uint32_t(n));
            size_t s = o.size();
            o.insert(o.end(), tp, tp + 4);
            if (n) o.insert(o.end(), p, p + n);
            put32(o, crc32(o.data() + s, 4 + n));
        };

        const size_t stride = 1 + size_t(width) * 4;
        std::vector<uint8_t> raw;
        raw.reserve(stride * size_t(height));
        for (int y = 0; y < height; ++y) {
            raw.push_back(0u);
            const uint32_t* row = pixels.data() + size_t(y) * size_t(width);
            for (int x = 0; x < width; ++x) {
                uint32_t p = row[x];
                raw.push_back(uint8_t(p >> 16));
                raw.push_back(uint8_t(p >> 8));
                raw.push_back(uint8_t(p));
                raw.push_back(255u);
            }
        }

        uint32_t s1 = 1u, s2 = 0u;
        for (uint8_t b : raw) {
            s1 += b; if (s1 >= 65521u) s1 -= 65521u;
            s2 += s1; if (s2 >= 65521u) s2 -= 65521u;
        }

        std::vector<uint8_t> zs;
        zs.reserve(raw.size() / 16 + 1024);
        zs.push_back(0x78u); zs.push_back(0x9Cu);
        uint64_t bb = 0; int bc = 0;
        auto bits = [&](uint32_t v, int nb) {
            bb |= (uint64_t(v) << bc); bc += nb;
            while (bc >= 8) { zs.push_back(uint8_t(bb)); bb >>= 8; bc -= 8; }
        };
        auto huff = [&](uint32_t code, int nb) {
            uint32_t r = 0;
            for (int i = 0; i < nb; ++i) r = (r << 1) | ((code >> i) & 1u);
            bits(r, nb);
        };
        auto lit = [&](int s) {
            if (s <= 143) huff(0x30u + uint32_t(s), 8);
            else if (s <= 255) huff(0x190u + uint32_t(s - 144), 9);
            else if (s <= 279) huff(uint32_t(s - 256), 7);
            else huff(0xC0u + uint32_t(s - 280), 8);
        };
        static constexpr int LB[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static constexpr int LE[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static constexpr int DB[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        static constexpr int DE[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

        bits(1u, 1); bits(1u, 2);
        std::array<int, 4096> head; head.fill(-1);
        const size_t n = raw.size();
        for (size_t i = 0; i < n;) {
            int bl = 0, bd = 0;
            if (i + 4 <= n) {
                uint32_t sig = uint32_t(raw[i]) | (uint32_t(raw[i+1]) << 8) | (uint32_t(raw[i+2]) << 16) | (uint32_t(raw[i+3]) << 24);
                int h = int((sig * 2654435761u) >> 20);
                int cands[3] = { i >= 4 ? int(i - 4) : -1, (i >= stride && stride <= 32768) ? int(i - stride) : -1, head[size_t(h)] };
                head[size_t(h)] = int(i);
                int ml = int(std::min<size_t>(258u, n - i));
                for (int cd : cands) {
                    if (cd < 0) continue;
                    int d = int(i) - cd;
                    if (d <= 0 || d > 32768 || raw[size_t(cd + bl)] != raw[i + size_t(bl)]) continue;
                    int l = 0;
                    while (l < ml && raw[size_t(cd + l)] == raw[i + size_t(l)]) ++l;
                    if (l > bl) { bl = l; bd = d; if (bl == 258) break; }
                }
            }
            if (bl >= 4) {
                int lc = 28; while (lc > 0 && bl < LB[lc]) --lc;
                lit(257 + lc); if (LE[lc]) bits(uint32_t(bl - LB[lc]), LE[lc]);
                int dc = 29; while (dc > 0 && bd < DB[dc]) --dc;
                huff(uint32_t(dc), 5); if (DE[dc]) bits(uint32_t(bd - DB[dc]), DE[dc]);
                i += size_t(bl);
            } else {
                lit(raw[i++]);
            }
        }
        lit(256);
        if (bc > 0) zs.push_back(uint8_t(bb));
        put32(zs, (s2 << 16) | s1);

        std::vector<uint8_t> png = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
        uint8_t ihdr[13] = {
            uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width),
            uint8_t(height >> 24), uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height),
            8u, 6u, 0u, 0u, 0u
        };
        chunk(png, "IHDR", ihdr, 13);
        chunk(png, "IDAT", zs.data(), zs.size());
        chunk(png, "IEND", nullptr, 0);
        return png;
    }

    bool save_png(const std::string& path) const {
        auto b = encode_png();
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return static_cast<bool>(f);
    }
};

} // namespace flux
