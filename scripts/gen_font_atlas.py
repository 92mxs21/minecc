#!/usr/bin/env python3
"""Generates a single unified 512x512 GPU Texture Atlas (`native/generated/font_atlas.hpp`)
containing:
  1. White solid texel `(2, 2)` for batched single-draw-call solid/gradient quads.
  2. 8x-supersampled Lucide-style crisp vector icons (`16px` & `20px`) with round caps/joins.
  3. 12 FreeType2-hinted DejaVu Sans & DejaVu Sans Mono faces (`11px`..`22px`) for razor-sharp UI & code.
"""

from __future__ import annotations

import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT_HPP = ROOT / "native/generated/font_atlas.hpp"

ATLAS_W = 512
ATLAS_H = 1024

SANS_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
SANS_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
MONO_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
MONO_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"

FACES = [
    ("sans_11", False, False, 11, SANS_REG),
    ("sans_12", False, False, 12, SANS_REG),
    ("sans_13", False, False, 13, SANS_REG),
    ("sans_14", False, False, 14, SANS_REG),
    ("sans_16", False, False, 16, SANS_REG),
    ("sans_b11", False, True, 11, SANS_BOLD),
    ("sans_b12", False, True, 12, SANS_BOLD),
    ("sans_b13", False, True, 13, SANS_BOLD),
    ("sans_b15", False, True, 15, SANS_BOLD),
    ("sans_b18", False, True, 18, SANS_BOLD),
    ("mono_12", True, False, 12, MONO_REG),
    ("mono_13", True, False, 13, MONO_REG),
    ("mono_b12", True, True, 12, MONO_BOLD),
    ("mono_b13", True, True, 13, MONO_BOLD),
]

CODEPOINTS = [cp for cp in range(32, 127)] + [0x00B7, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2192, 0x2191, 0x2193, 0x2713, 0x00D7]

ICON_NAMES = [
    "logo", "chevron_down", "chevron_right", "arrow_left", "search",
    "git_compare", "cpu", "download", "refresh", "folder",
    "file_code", "close", "check", "sliders", "zap",
    "layers", "terminal", "spark", "plus", "minus",
    "rrect_fill", "rrect_stroke",
]


def draw_lucide_icon(name: str, out_px: int = 20) -> Image.Image:
    """Render an 8x-supersampled crisp vector icon onto a `24x24` coordinate grid."""
    ss = 8
    N = out_px * ss
    img = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(img)
    s = N / 24.0
    sw = max(2, round(1.95 * s))

    def P(x: float, y: float) -> tuple[float, float]:
        return (x * s, y * s)

    def seg(pts: list[tuple[float, float]], w: int = sw) -> None:
        coords = [P(x, y) for x, y in pts]
        d.line(coords, fill=255, width=w, joint="curve")
        r = w * 0.48
        for cx, cy in (coords[0], coords[-1]):
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)

    def circ(cx: float, cy: float, r: float, fill: int | None = None, outline: int = 255, w: int = sw) -> None:
        x0, y0 = P(cx - r, cy - r)
        x1, y1 = P(cx + r, cy + r)
        d.ellipse((x0, y0, x1, y1), fill=fill, outline=outline, width=w)

    def rrect(x0: float, y0: float, x1: float, y1: float, rad: float, fill: int | None = None, w: int = sw) -> None:
        a, b = P(x0, y0), P(x1, y1)
        d.rounded_rectangle((a[0], a[1], b[0], b[1]), radius=rad * s, fill=fill, outline=255, width=w)

    if name == "logo":
        rrect(3.5, 3.5, 20.5, 20.5, 4.5)
        seg([(8.0, 15.5), (8.0, 8.5), (12.0, 13.0), (16.0, 8.5), (16.0, 15.5)])
    elif name == "chevron_down":
        seg([(6, 9), (12, 15), (18, 9)])
    elif name == "chevron_right":
        seg([(9, 6), (15, 12), (9, 18)])
    elif name == "arrow_left":
        seg([(19, 12), (5, 12)])
        seg([(11, 6), (5, 12), (11, 18)])
    elif name == "search":
        circ(11, 11, 6.8)
        seg([(16.0, 16.0), (20.5, 20.5)])
    elif name == "git_compare":
        circ(6, 6, 2.6)
        circ(18, 18, 2.6)
        seg([(6, 8.6), (6, 16), (11, 18)])
        seg([(18, 15.4), (18, 8), (13, 6)])
    elif name == "cpu":
        rrect(5, 5, 19, 19, 2.5)
        rrect(9, 9, 15, 15, 1.0)
        for k in (9, 15):
            seg([(k, 2), (k, 5)])
            seg([(k, 19), (k, 22)])
            seg([(2, k), (5, k)])
            seg([(19, k), (22, k)])
    elif name == "download":
        seg([(12, 3.5), (12, 15)])
        seg([(7.5, 10.8), (12, 15.3), (16.5, 10.8)])
        seg([(4.5, 19.5), (19.5, 19.5)])
    elif name == "refresh":
        d.arc((*P(4, 4), *P(20, 20)), start=205, end=505, fill=255, width=sw)
        seg([(16.2, 4.2), (19.8, 7.6), (15.4, 8.4)])
    elif name == "folder":
        seg([(3, 6.5), (9.2, 6.5), (11.2, 8.8), (21, 8.8), (21, 18.8), (3, 18.8), (3, 6.5)])
    elif name == "file_code":
        rrect(4.5, 3, 19.5, 21, 2.2)
        seg([(10, 10), (7.6, 12.5), (10, 15)])
        seg([(14, 10), (16.4, 12.5), (14, 15)])
    elif name == "close":
        seg([(6.5, 6.5), (17.5, 17.5)])
        seg([(17.5, 6.5), (6.5, 17.5)])
    elif name == "check":
        seg([(4.8, 12.5), (9.6, 17.2), (19.2, 7.2)])
    elif name == "sliders":
        seg([(4, 7), (20, 7)])
        seg([(4, 17), (20, 17)])
        circ(9, 7, 2.4, fill=255)
        circ(15, 17, 2.4, fill=255)
    elif name == "zap":
        d.polygon([P(13, 2.5), P(4.8, 13.5), P(11.8, 13.5), P(10.8, 21.5), P(19.2, 10.5), P(12.2, 10.5)], fill=255)
    elif name == "layers":
        seg([(12, 3.5), (21, 8.2), (12, 12.9), (3, 8.2), (12, 3.5)])
        seg([(3, 12.8), (12, 17.5), (21, 12.8)])
        seg([(3, 16.8), (12, 21.3), (21, 16.8)])
    elif name == "terminal":
        rrect(3, 4, 21, 20, 2.5)
        seg([(7, 9), (10.5, 12), (7, 15)])
        seg([(12.5, 15), (17, 15)])
    elif name == "spark":
        pts = [P(12, 2.5), P(14.3, 9.7), P(21.5, 12), P(14.3, 14.3), P(12, 21.5), P(9.7, 14.3), P(2.5, 12), P(9.7, 9.7)]
        d.polygon(pts, fill=255)
    elif name == "plus":
        seg([(12, 5.5), (12, 18.5)])
        seg([(5.5, 12), (18.5, 12)])
    elif name == "minus":
        seg([(5.5, 12), (18.5, 12)])
    elif name == "rrect_fill":
        d.rounded_rectangle((0, 0, N - 1, N - 1), radius=round(N * 0.35), fill=255)
    elif name == "rrect_stroke":
        bw = max(2, round(ss * 1.15))
        d.rounded_rectangle((bw // 2, bw // 2, N - 1 - bw // 2, N - 1 - bw // 2), radius=round(N * 0.35), fill=None, outline=255, width=bw)

    return img.resize((out_px, out_px), Image.Resampling.LANCZOS)


class ShelfPacker:
    def __init__(self, w: int, h: int) -> None:
        self.w = w
        self.h = h
        self.atlas = Image.new("L", (w, h), 0)
        self.cur_x = 1
        self.cur_y = 1
        self.row_h = 0

    def pack(self, sub: Image.Image) -> tuple[int, int]:
        sw, sh = sub.size
        if sw == 0 or sh == 0:
            return (0, 0)
        if self.cur_x + sw + 2 >= self.w:
            self.cur_x = 1
            self.cur_y += self.row_h + 2
            self.row_h = 0
        if self.cur_y + sh + 1 >= self.h:
            raise RuntimeError(f"Atlas overflow at ({self.cur_x}, {self.cur_y})!")
        x, y = self.cur_x, self.cur_y
        self.atlas.paste(sub, (x, y))
        self.cur_x += sw + 2
        self.row_h = max(self.row_h, sh)
        return (x, y)


def main() -> None:
    packer = ShelfPacker(ATLAS_W, ATLAS_H)

    # 1. Pack 6x6 solid white block at (1, 1) -> center (3, 3) is safe 1.0 alpha under GL_LINEAR!
    white_px = packer.pack(Image.new("L", (6, 6), 255))

    # 2. Pack crisp 8x-supersampled icons at BOTH 16px and 20px!
    icon_entries = []
    for name in ICON_NAMES:
        for sz in (16, 20):
            im = draw_lucide_icon(name, sz)
            ix, iy = packer.pack(im)
            icon_entries.append((name, sz, ix, iy, sz, sz))

    # 3. Pack all 14 FreeType2 font faces
    face_meta = []
    glyphs = []
    for fname, is_mono, is_bold, px, font_path in FACES:
        fnt = ImageFont.truetype(font_path, px)
        ascent, descent = fnt.getmetrics()
        start_idx = len(glyphs)
        for cp in CODEPOINTS:
            ch = chr(cp)
            adv = float(fnt.getlength(ch))
            mask = fnt.getmask(ch, mode="L")
            mw, mh = mask.size
            bx, by = fnt.getbbox(ch)[:2] if (mw > 0 and mh > 0) else (0, 0)
            if mw > 0 and mh > 0:
                gim = Image.new("L", (mw, mh), 0)
                gim.putdata(list(mask))
                tx, ty = packer.pack(gim)
            else:
                tx, ty = 0, 0
            glyphs.append((cp, mw, mh, bx, by, adv, tx, ty))
        face_meta.append((fname, is_mono, is_bold, px, ascent, descent, ascent + descent + 3, start_idx, len(glyphs) - start_idx))

    raw = packer.atlas.tobytes()
    OUT_HPP.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "#pragma once",
        "// Auto-generated 512x512 Unified GPU Font + 8x-Supersampled Vector Icon Atlas",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace flux::atlas {",
        "",
        f"inline constexpr int TEX_W = {ATLAS_W};",
        f"inline constexpr int TEX_H = {ATLAS_H};",
        f"inline constexpr float WHITE_U = {(white_px[0] + 3.0) / ATLAS_W:.6f}f;",
        f"inline constexpr float WHITE_V = {(white_px[1] + 3.0) / ATLAS_H:.6f}f;",
        "",
        "struct Glyph {",
        "    uint32_t cp;",
        "    int16_t w, h, bx, by;",
        "    float advance;",
        "    uint16_t tx, ty;",
        "};",
        "",
        "struct Face {",
        "    bool mono, bold;",
        "    int16_t px, ascent, descent, line_h;",
        "    uint32_t first, count;",
        "};",
        "",
        "struct IconSprite {",
        "    const char* name;",
        "    int16_t size;",
        "    uint16_t tx, ty;",
        "};",
        "",
        f"inline constexpr IconSprite ICONS[{len(icon_entries)}] = {{",
    ]
    for name, sz, ix, iy, _, _ in icon_entries:
        lines.append(f'    {{"{name}", {sz}, {ix}, {iy}}},')
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr Face FACES[{len(face_meta)}] = {{")
    for _, mono, bold, px, asc, desc, lh, st, cnt in face_meta:
        lines.append(f"    {{{'true' if mono else 'false'}, {'true' if bold else 'false'}, {px}, {asc}, {desc}, {lh}, {st}u, {cnt}u}},")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr Glyph GLYPHS[{len(glyphs)}] = {{")
    for cp, mw, mh, bx, by, adv, tx, ty in glyphs:
        lines.append(f"    {{{cp}u, {mw}, {mh}, {bx}, {by}, {adv:.3f}f, {tx}u, {ty}u}},")
    lines.append("};")
    lines.append("")

    # Store run-length compressed alpha atlas so `.rodata` stays tiny (~65 KiB instead of 262 KiB)!
    rle = bytearray()
    n = len(raw)
    i = 0
    while i < n:
        v = raw[i]
        run = 1
        while i + run < n and raw[i + run] == v and run < 255:
            run += 1
        rle.append(run)
        rle.append(v)
        i += run

    lines.append(f"inline constexpr uint32_t ATLAS_RLE_LEN = {len(rle)}u;")
    lines.append(f"inline constexpr uint8_t ATLAS_RLE[{len(rle)}] = {{")
    for row_start in range(0, len(rle), 24):
        chunk = ", ".join(f"0x{b:02X}u" for b in rle[row_start : row_start + 24])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    lines.append(
        """inline const uint8_t* unpacked_alpha() noexcept {
    static uint8_t buf[TEX_W * TEX_H];
    static bool ready = false;
    if (!ready) {
        size_t out = 0;
        for (size_t i = 0; i + 1 < ATLAS_RLE_LEN && out < TEX_W * TEX_H; i += 2) {
            uint8_t cnt = ATLAS_RLE[i], val = ATLAS_RLE[i + 1];
            for (uint8_t k = 0; k < cnt && out < TEX_W * TEX_H; ++k) buf[out++] = val;
        }
        ready = true;
    }
    return buf;
}

inline int select_face(bool mono, bool bold, int px) noexcept {
    int best = 0, best_err = 999999;
    for (int i = 0; i < int(sizeof(FACES) / sizeof(FACES[0])); ++i) {
        if (FACES[i].mono != mono || FACES[i].bold != bold) continue;
        int err = FACES[i].px > px ? (FACES[i].px - px) : (px - FACES[i].px);
        if (err < best_err) { best_err = err; best = i; }
    }
    return best;
}

inline const Glyph* find_glyph(int face_idx, uint32_t cp) noexcept {
    const Face& f = FACES[face_idx];
    uint32_t lo = f.first, hi = f.first + f.count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (GLYPHS[mid].cp == cp) return &GLYPHS[mid];
        if (GLYPHS[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

inline const IconSprite* find_icon(std::string_view name, int want_sz = 20) noexcept {
    int target = (want_sz <= 17) ? 16 : 20;
    for (const auto& ic : ICONS) {
        if (ic.size == target && name == ic.name) return &ic;
    }
    return &ICONS[0];
}

} // namespace flux::atlas"""
    )

    OUT_HPP.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[gen_font_atlas] Packed {len(icon_entries)} crisp 8x icons + {len(glyphs)} glyphs into {ATLAS_W}x{ATLAS_H} ({len(rle):,} bytes RLE) -> {OUT_HPP}")


if __name__ == "__main__":
    main()
