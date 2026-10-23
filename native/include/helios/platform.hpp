#pragma once
// HeliosPlatform: Unified Native Windowing (Win32 GDI/DWM + Linux X11 dlopen) + Input/DOM Hit-Test + Remote Framebuffer Server

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "native/include/helios/bundle_format.hpp"
#include "native/include/helios/css_engine.hpp"
#include "native/include/helios/html_dom.hpp"
#include "native/include/helios/js_vm.hpp"
#include "native/include/helios/painter.hpp"

namespace helios {

class HeliosEngine {
public:
    std::shared_ptr<DomNode> doc_root;
    Stylesheet stylesheet;
    std::unique_ptr<js::JsRuntime> js_rt;
    SoftwarePainter painter;

    int viewport_w = 1360;
    int viewport_h = 840;
    std::shared_ptr<DomNode> hovered_leaf;
    std::shared_ptr<DomNode> focused_input;
    double last_paint_ms = 0.0;
    uint64_t frame_seq = 0;
    std::string clipboard_text;

    void load_bundle(const bundle::BundlePayload& bundle) {
        auto parsed = parse_html_document(bundle.html_source);
        doc_root = parsed.root;
        std::string all_css = bundle.css_source;
        for (const auto& st : parsed.inline_styles) all_css += "\n" + st;
        stylesheet = parse_stylesheet(all_css);
        js_rt = std::make_unique<js::JsRuntime>(doc_root, parsed.last_uid + 10);
        painter.resize(viewport_w, viewport_h);
    }

    void render_if_needed(bool force = false) {
        if (!doc_root) return;
        if (!force && js_rt && !js_rt->dom_dirty) return;
        auto t0 = std::chrono::steady_clock::now();
        if (painter.width != viewport_w || painter.height != viewport_h) {
            painter.resize(viewport_w, viewport_h);
        }
        relayout_document(doc_root, stylesheet, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        painter.fill_rect(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h), 0xFF121212u);
        painter.paint_dom_node(doc_root);
        if (js_rt) js_rt->dom_dirty = false;
        auto t1 = std::chrono::steady_clock::now();
        last_paint_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ++frame_seq;
    }

    std::shared_ptr<DomNode> hit_test_node(const std::shared_ptr<DomNode>& node, float px, float py, float offset_y = 0.0f) {
        if (!node || node->style.display_none) return nullptr;
        float ny = (node->style.position_mode == 1) ? node->y : (node->y - offset_y);
        if (node->type == NodeType::Element && node->tag != "#document") {
            if (px < node->x || px > node->x + node->w || py < ny || py > ny + node->h) {
                return nullptr;
            }
        }
        // Check fixed/absolute overlays first (painted on top)
        for (size_t i = node->children.size(); i > 0; --i) {
            const auto& ch = node->children[i - 1];
            if (!ch->style.display_none && ch->style.position_mode == 1) {
                if (auto h = hit_test_node(ch, px, py, 0.0f)) return h;
            }
        }
        float child_scroll = offset_y + node->scroll_y;
        for (size_t i = node->children.size(); i > 0; --i) {
            const auto& ch = node->children[i - 1];
            if (!ch->style.display_none && ch->style.position_mode != 1) {
                if (auto h = hit_test_node(ch, px, py, child_scroll)) return h;
            }
        }
        if (node->type == NodeType::Text) {
            return node->parent.lock();
        }
        return node;
    }

    static void clear_hover_flags(const std::shared_ptr<DomNode>& n) {
        if (!n) return;
        n->hovered = false;
        for (auto& c : n->children) clear_hover_flags(c);
    }

    bool on_mouse_move(float mx, float my) {
        auto hit = hit_test_node(doc_root, mx, my);
        if (hit != hovered_leaf) {
            clear_hover_flags(doc_root);
            hovered_leaf = hit;
            auto cur = hit;
            while (cur) {
                cur->hovered = true;
                cur = cur->parent.lock();
            }
            if (js_rt) js_rt->dom_dirty = true;
            render_if_needed();
        }
        return hit ? hit->style.cursor_pointer : false;
    }

    void on_mouse_click(float mx, float my) {
        auto hit = hit_test_node(doc_root, mx, my);
        if (focused_input && focused_input != hit) {
            focused_input->focused = false;
            focused_input = nullptr;
            if (js_rt) js_rt->dom_dirty = true;
        }
        if (hit && hit->tag == "input") {
            focused_input = hit;
            hit->focused = true;
            if (js_rt) js_rt->dom_dirty = true;
        }
        if (hit && js_rt) {
            js_rt->dispatch_dom_event("click", hit);
        }
        render_if_needed();
    }

    void on_mouse_wheel(float mx, float my, float delta_px) {
        auto hit = hit_test_node(doc_root, mx, my);
        auto cur = hit;
        while (cur) {
            if (cur->style.overflow == 2) {
                float inner_h = cur->h - cur->style.pad_t - cur->style.pad_b - cur->style.bdr_t - cur->style.bdr_b;
                float max_s = std::max(0.0f, cur->content_h - inner_h);
                if (max_s > 0.5f) {
                    cur->scroll_y = std::clamp(cur->scroll_y + delta_px, 0.0f, max_s);
                    if (js_rt) js_rt->dom_dirty = true;
                    render_if_needed();
                    return;
                }
            }
            cur = cur->parent.lock();
        }
    }

    void on_key_input(const std::string& key_str, bool printable) {
        if (focused_input) {
            std::string val = focused_input->get_attr("value");
            if (key_str == "Backspace") {
                while (!val.empty() && ((static_cast<unsigned char>(val.back()) & 0xC0u) == 0x80u)) val.pop_back();
                if (!val.empty()) val.pop_back();
                focused_input->set_attr("value", val);
                if (js_rt) {
                    js_rt->dom_dirty = true;
                    js_rt->dispatch_dom_event("input", focused_input);
                }
            } else if (key_str == "Escape") {
                focused_input->focused = false;
                focused_input = nullptr;
                if (js_rt) {
                    js_rt->dom_dirty = true;
                    js_rt->dispatch_dom_event("keydown", doc_root, "Escape");
                }
            } else if (key_str == "Enter") {
                if (js_rt) js_rt->dispatch_dom_event("keydown", focused_input, "Enter");
            } else if (printable && !key_str.empty()) {
                val += key_str;
                focused_input->set_attr("value", val);
                if (js_rt) {
                    js_rt->dom_dirty = true;
                    js_rt->dispatch_dom_event("input", focused_input);
                }
            }
        } else if (js_rt) {
            js_rt->dispatch_dom_event("keydown", doc_root, key_str);
        }
        render_if_needed();
    }

    void on_resize(int w, int h) {
        viewport_w = std::max(640, w);
        viewport_h = std::max(420, h);
        render_if_needed(true);
    }
};

// ============================================================================
// Platform Native Dialog & Clipboard Helpers
// ============================================================================
inline std::string native_open_jar_dialog() {
#ifdef _WIN32
    wchar_t file_buf[1024] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = L"Java Archives (*.jar;*.zip)\0*.jar;*.zip\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        int needed = WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, nullptr, 0, nullptr, nullptr);
        if (needed > 1) {
            std::string out(static_cast<size_t>(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, out.data(), needed, nullptr, nullptr);
            return out;
        }
    }
    return "";
#else
    // On Linux desktop, invoke zenity/kdialog if installed; otherwise UI prompt modal accepts path directly
    FILE* pipe = popen("zenity --file-selection --title='Open Minecraft JAR (26.3.jar)' --file-filter='*.jar *.zip' 2>/dev/null", "r");
    if (!pipe) return "";
    char buf[1024];
    std::string out;
    if (fgets(buf, sizeof(buf), pipe)) out = buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

inline void native_set_clipboard(HeliosEngine& eng, const std::string& text) {
    eng.clipboard_text = text;
#ifdef _WIN32
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen) * sizeof(wchar_t));
            if (hmem) {
                wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hmem));
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, dst, wlen);
                GlobalUnlock(hmem);
                SetClipboardData(CF_UNICODETEXT, hmem);
            }
        }
        CloseClipboard();
    }
#endif
}

// ============================================================================
// Platform 1: Native Windows Win32 Desktop GUI (`StretchDIBits` + Dark DWM)
// ============================================================================
#ifdef _WIN32
inline LRESULT CALLBACK helios_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* eng = reinterpret_cast<HeliosEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!eng) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // Prevent GDI background flicker
        case WM_SIZE: {
            int nw = LOWORD(lp);
            int nh = HIWORD(lp);
            if (nw > 0 && nh > 0) {
                eng->on_resize(nw, nh);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            float mx = static_cast<float>(static_cast<short>(LOWORD(lp)));
            float my = static_cast<float>(static_cast<short>(HIWORD(lp)));
            uint64_t prev_seq = eng->frame_seq;
            bool hand = eng->on_mouse_move(mx, my);
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(hand ? 32649 : 32512)));
            if (eng->frame_seq != prev_seq) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float mx = static_cast<float>(static_cast<short>(LOWORD(lp)));
            float my = static_cast<float>(static_cast<short>(HIWORD(lp)));
            eng->on_mouse_click(mx, my);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(hwnd, &pt);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            eng->on_mouse_wheel(static_cast<float>(pt.x), static_cast<float>(pt.y), -static_cast<float>(delta) * 0.45f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wp == VK_BACK) { eng->on_key_input("Backspace", false); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wp == VK_ESCAPE) { eng->on_key_input("Escape", false); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wp == VK_RETURN) { eng->on_key_input("Enter", false); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                eng->on_key_input("/", false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            wchar_t wc = static_cast<wchar_t>(wp);
            if (wc >= 32 && wc != 127) {
                char utf8[8] = {0};
                WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
                eng->on_key_input(utf8, true);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            eng->render_if_needed();
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = eng->painter.width;
            bmi.bmiHeader.biHeight = -eng->painter.height; // Top-down scanlines!
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(
                hdc, 0, 0, eng->painter.width, eng->painter.height,
                0, 0, eng->painter.width, eng->painter.height,
                eng->painter.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline int run_native_desktop_window(HeliosEngine& eng, const std::string& title) {
    SetProcessDPIAware();
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = helios_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"MineccStudioHeliosWnd";
    RegisterClassExW(&wc);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wtitle(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), wlen);

    RECT wr{0, 0, eng.viewport_w, eng.viewport_h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hinst, &eng);
    if (!hwnd) return 1;

    BOOL dark_mode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark_mode, sizeof(dark_mode));

    eng.render_if_needed(true);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
#else
// ============================================================================
// Platform 2: Native Linux X11 Desktop Window via Dynamic Xlib (`libX11.so.6`)
// ============================================================================
namespace x11_dyn {
struct Display;
using XID = unsigned long;
using Window = XID;
using Cursor = XID;
using Atom = unsigned long;
using GC = void*;
using Visual = void;

struct XImage {
    int width, height;
    int xoffset, format;
    char* data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    char* obdata;
    void* funcs[8];
};

struct XEvent {
    int type;
    unsigned long pad_serial;
    int send_event;
    Display* display;
    Window window;
    unsigned char raw[192];
};
struct XButtonEvent {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned int state, button; int same_screen;
};
struct XMotionEvent {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned int state; char is_hint; int same_screen;
};
struct XConfigureEvent {
    int type; unsigned long serial; int send_event; Display* display; Window event, window;
    int x, y, width, height, border_width; Window above; int override_redirect;
};
struct XClientMessageEvent {
    int type; unsigned long serial; int send_event; Display* display; Window window;
    Atom message_type; int format; long l[5];
};
} // namespace x11_dyn

inline int run_native_desktop_window(HeliosEngine& eng, const std::string& title) {
    using namespace x11_dyn;
    void* libx11 = dlopen("libX11.so.6", RTLD_LAZY);
    if (!libx11) {
        std::cerr << "[minecc-studio] libX11.so.6 not available; use --serve 0.0.0.0:8765 or --render-png <out.png>\n";
        return 1;
    }

    auto fn_open = reinterpret_cast<Display* (*)(const char*)>(dlsym(libx11, "XOpenDisplay"));
    auto fn_close = reinterpret_cast<int (*)(Display*)>(dlsym(libx11, "XCloseDisplay"));
    auto fn_def_scr = reinterpret_cast<int (*)(Display*)>(dlsym(libx11, "XDefaultScreen"));
    auto fn_root = reinterpret_cast<Window (*)(Display*, int)>(dlsym(libx11, "XRootWindow"));
    auto fn_vis = reinterpret_cast<Visual* (*)(Display*, int)>(dlsym(libx11, "XDefaultVisual"));
    auto fn_depth = reinterpret_cast<int (*)(Display*, int)>(dlsym(libx11, "XDefaultDepth"));
    auto fn_gc = reinterpret_cast<GC (*)(Display*, int)>(dlsym(libx11, "XDefaultGC"));
    auto fn_create_win = reinterpret_cast<Window (*)(Display*, Window, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)>(dlsym(libx11, "XCreateSimpleWindow"));
    auto fn_store_name = reinterpret_cast<int (*)(Display*, Window, const char*)>(dlsym(libx11, "XStoreName"));
    auto fn_select = reinterpret_cast<int (*)(Display*, Window, long)>(dlsym(libx11, "XSelectInput"));
    auto fn_map = reinterpret_cast<int (*)(Display*, Window)>(dlsym(libx11, "XMapWindow"));
    auto fn_intern = reinterpret_cast<Atom (*)(Display*, const char*, int)>(dlsym(libx11, "XInternAtom"));
    auto fn_set_proto = reinterpret_cast<int (*)(Display*, Window, Atom*, int)>(dlsym(libx11, "XSetWMProtocols"));
    auto fn_next_ev = reinterpret_cast<int (*)(Display*, XEvent*)>(dlsym(libx11, "XNextEvent"));
    auto fn_pending = reinterpret_cast<int (*)(Display*)>(dlsym(libx11, "XPending"));
    auto fn_create_img = reinterpret_cast<XImage* (*)(Display*, Visual*, unsigned int, int, int, char*, unsigned int, unsigned int, int, int)>(dlsym(libx11, "XCreateImage"));
    auto fn_put_img = reinterpret_cast<int (*)(Display*, Window, GC, XImage*, int, int, int, int, unsigned int, unsigned int)>(dlsym(libx11, "XPutImage"));
    auto fn_lookup = reinterpret_cast<int (*)(void*, char*, int, unsigned long*, void*)>(dlsym(libx11, "XLookupString"));
    auto fn_font_cur = reinterpret_cast<Cursor (*)(Display*, unsigned int)>(dlsym(libx11, "XCreateFontCursor"));
    auto fn_def_cur = reinterpret_cast<int (*)(Display*, Window, Cursor)>(dlsym(libx11, "XDefineCursor"));

    Display* dpy = fn_open ? fn_open(nullptr) : nullptr;
    if (!dpy) {
        std::cerr << "[minecc-studio] No X11 DISPLAY open. Run with `--serve 0.0.0.0:8765` for live interactive mode or `--render-png <file.png>`.\n";
        dlclose(libx11);
        return 1;
    }

    int scr = fn_def_scr(dpy);
    Window win = fn_create_win(dpy, fn_root(dpy, scr), 40, 40,
                               static_cast<unsigned int>(eng.viewport_w),
                               static_cast<unsigned int>(eng.viewport_h),
                               0, 0x121212ul, 0x121212ul);
    fn_store_name(dpy, win, title.c_str());
    // ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask | StructureNotifyMask
    fn_select(dpy, win, (1L << 15) | (1L << 0) | (1L << 2) | (1L << 6) | (1L << 17));
    Atom wm_delete = fn_intern(dpy, "WM_DELETE_WINDOW", 0);
    fn_set_proto(dpy, win, &wm_delete, 1);
    Cursor cur_arrow = fn_font_cur(dpy, 68u /* XC_left_ptr */);
    Cursor cur_hand = fn_font_cur(dpy, 60u /* XC_hand2 */);
    fn_map(dpy, win);

    auto blit = [&]() {
        eng.render_if_needed();
        XImage* img = fn_create_img(
            dpy, fn_vis(dpy, scr), static_cast<unsigned int>(fn_depth(dpy, scr)),
            2 /* ZPixmap */, 0, reinterpret_cast<char*>(eng.painter.pixels.data()),
            static_cast<unsigned int>(eng.painter.width), static_cast<unsigned int>(eng.painter.height),
            32, eng.painter.width * 4);
        if (img) {
            fn_put_img(dpy, win, fn_gc(dpy, scr), img, 0, 0, 0, 0,
                       static_cast<unsigned int>(eng.painter.width),
                       static_cast<unsigned int>(eng.painter.height));
            img->data = nullptr;
            std::free(img);
        }
    };

    eng.render_if_needed(true);
    blit();

    bool running = true;
    while (running) {
        XEvent ev{};
        fn_next_ev(dpy, &ev);
        do {
            if (ev.type == 12 /* Expose */) {
                blit();
            } else if (ev.type == 22 /* ConfigureNotify */) {
                auto* ce = reinterpret_cast<XConfigureEvent*>(&ev);
                if (ce->width != eng.viewport_w || ce->height != eng.viewport_h) {
                    eng.on_resize(ce->width, ce->height);
                    blit();
                }
            } else if (ev.type == 6 /* MotionNotify */) {
                auto* me = reinterpret_cast<XMotionEvent*>(&ev);
                bool hand = eng.on_mouse_move(static_cast<float>(me->x), static_cast<float>(me->y));
                fn_def_cur(dpy, win, hand ? cur_hand : cur_arrow);
            } else if (ev.type == 4 /* ButtonPress */) {
                auto* be = reinterpret_cast<XButtonEvent*>(&ev);
                if (be->button == 1) eng.on_mouse_click(static_cast<float>(be->x), static_cast<float>(be->y));
                else if (be->button == 4) eng.on_mouse_wheel(static_cast<float>(be->x), static_cast<float>(be->y), -56.0f);
                else if (be->button == 5) eng.on_mouse_wheel(static_cast<float>(be->x), static_cast<float>(be->y), +56.0f);
            } else if (ev.type == 2 /* KeyPress */) {
                char buf[16] = {0};
                unsigned long ks = 0;
                int n = fn_lookup(&ev, buf, sizeof(buf) - 1, &ks, nullptr);
                if (ks == 0xFF08ul) eng.on_key_input("Backspace", false);
                else if (ks == 0xFF1Bul) eng.on_key_input("Escape", false);
                else if (ks == 0xFF0Dul) eng.on_key_input("Enter", false);
                else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) eng.on_key_input(std::string(buf, static_cast<size_t>(n)), true);
            } else if (ev.type == 33 /* ClientMessage */) {
                auto* cm = reinterpret_cast<XClientMessageEvent*>(&ev);
                if (static_cast<Atom>(cm->l[0]) == wm_delete) running = false;
            }
        } while (running && fn_pending(dpy) > 0 && (fn_next_ev(dpy, &ev), true));

        if (running) blit();
    }

    fn_close(dpy);
    dlclose(libx11);
    return 0;
}
#endif

// ============================================================================
// Platform 3: Live Native C++ Software Framebuffer Streaming & Executable Download Server
// Streams genuine pixels rasterized in C++ by `SoftwarePainter` + forwards input 1:1
// ============================================================================
inline int run_remote_framebuffer_server(HeliosEngine& eng, const std::string& host_port, const std::string& bin_dir) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    using SockT = SOCKET;
    auto close_sock = [](SockT s) { closesocket(s); };
#else
    using SockT = int;
    auto close_sock = [](SockT s) { close(s); };
#endif

    int port = 8765;
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) port = std::atoi(host_port.substr(colon + 1).c_str());

    SockT srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::cerr << "Failed to open listening socket on port " << port << "\n";
        return 1;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0 as required for live sandbox preview!
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(srv, 32) < 0) {
        std::cerr << "Failed to bind/listen on 0.0.0.0:" << port << "\n";
        close_sock(srv);
        return 1;
    }

    eng.render_if_needed(true);
    std::mutex eng_mtx;
    std::cout << "[minecc-studio] Helios C++20 Software Renderer listening on http://0.0.0.0:" << port << "\n" << std::flush;

    static const std::string VIEWER_SHELL = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>minecc-studio — Native C++20 Software Framebuffer Stream + Binary Downloads</title>
<style>
  html, body { margin: 0; padding: 0; height: 100%; background: #0c0d10; color: #d9e1eb; font-family: system-ui, -apple-system, sans-serif; overflow: hidden; display: flex; flex-direction: column; }
  .bar { height: 38px; background: #161920; border-bottom: 1px solid #2b313d; display: flex; align-items: center; justify-content: space-between; padding: 0 12px; font-size: 12px; gap: 10px; flex-shrink: 0; }
  .left, .right { display: flex; align-items: center; gap: 8px; }
  .pill { background: #1e3a2f; color: #56d364; border: 1px solid #2ea04366; border-radius: 999px; padding: 2px 8px; font-weight: 600; font-size: 11px; }
  .metric { color: #8b949e; font-family: monospace; font-size: 11px; }
  a.dl { color: #e6edf3; text-decoration: none; background: #21262d; border: 1px solid #30363d; padding: 4px 9px; border-radius: 5px; font-size: 11px; font-weight: 600; }
  a.dl.win { background: #1668dc; border-color: #3c89e8; }
  a.dl:hover { filter: brightness(1.12); }
  .stage { flex: 1; display: flex; align-items: center; justify-content: center; overflow: hidden; background: #090a0d; position: relative; }
  img#fb { max-width: 100%; max-height: 100%; display: block; user-select: none; -webkit-user-drag: none; box-shadow: 0 12px 40px rgba(0,0,0,0.65); border: 1px solid #282c34; }
</style>
</head>
<body>
  <div class="bar">
    <div class="left">
      <span class="pill">LIVE C++20 FRAMEBUFFER</span>
      <span>Streaming raw 32-bpp software pixels from <code>bin/minecc-studio-linux-x86_64</code> (Zero WebView / Zero Browser CSS Engine)</span>
      <span id="stat" class="metric"></span>
    </div>
    <div class="right">
      <a class="dl win" href="download/minecc-studio-windows-x86_64.exe">Download Windows .exe</a>
      <a class="dl" href="download/minecc-studio-linux-x86_64">Download Linux x64</a>
      <a class="dl" href="download/minecc-uic-windows-x86_64.exe">Compiler (.exe)</a>
      <a class="dl" href="download/minecc-uic-linux-x86_64">Compiler (Linux)</a>
    </div>
  </div>
  <div class="stage" id="stage">
    <img id="fb" src="api/frame.png?init=1" alt="Helios Native Software Framebuffer">
  </div>
<script>
const fb = document.getElementById('fb');
const stat = document.getElementById('stat');
let busy = false, queued = null;

function mapCoords(e) {
  const r = fb.getBoundingClientRect();
  const sx = fb.naturalWidth / Math.max(1, r.width);
  const sy = fb.naturalHeight / Math.max(1, r.height);
  return { x: Math.round((e.clientX - r.left) * sx), y: Math.round((e.clientY - r.top) * sy) };
}

async function sendEv(q) {
  if (busy) { queued = q; return; }
  busy = true;
  try {
    const res = await fetch('api/event?' + q);
    const hdr = res.headers.get('X-Helios-Paint-Ms') || '';
    const hand = res.headers.get('X-Helios-Cursor') || 'arrow';
    fb.style.cursor = hand === 'pointer' ? 'pointer' : 'default';
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const prev = fb.dataset.blobUrl;
    fb.onload = () => { if (prev) URL.revokeObjectURL(prev); };
    fb.dataset.blobUrl = url;
    fb.src = url;
    stat.textContent = 'C++ raster: ' + hdr + ' ms | ' + fb.naturalWidth + 'x' + fb.naturalHeight;
  } catch (_) {}
  busy = false;
  if (queued) { const n = queued; queued = null; sendEv(n); }
}

fb.addEventListener('click', e => { const c = mapCoords(e); sendEv(`type=click&x=${c.x}&y=${c.y}`); });
let lastMove = 0;
fb.addEventListener('mousemove', e => {
  const now = performance.now();
  if (now - lastMove < 55) return;
  lastMove = now;
  const c = mapCoords(e);
  sendEv(`type=move&x=${c.x}&y=${c.y}`);
});
fb.addEventListener('wheel', e => {
  e.preventDefault();
  const c = mapCoords(e);
  sendEv(`type=wheel&x=${c.x}&y=${c.y}&dy=${Math.round(e.deltaY * 0.7)}`);
}, { passive: false });
window.addEventListener('keydown', e => {
  if (e.metaKey || e.ctrlKey) return;
  if (e.key === 'Backspace' || e.key === 'Escape' || e.key === 'Enter' || e.key.length === 1) {
    e.preventDefault();
    sendEv('type=key&k=' + encodeURIComponent(e.key));
  }
});
</script>
</body>
</html>)HTML";

    auto query_param = [](const std::string& q, const std::string& key) -> std::string {
        size_t pos = 0;
        while (pos < q.size()) {
            size_t amp = q.find('&', pos);
            std::string part = (amp == std::string::npos) ? q.substr(pos) : q.substr(pos, amp - pos);
            size_t eq = part.find('=');
            if (eq != std::string::npos && part.substr(0, eq) == key) {
                std::string raw = part.substr(eq + 1);
                std::string dec;
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '+' ) dec.push_back(' ');
                    else if (raw[i] == '%' && i + 2 < raw.size()) {
                        dec.push_back(static_cast<char>(std::strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16)));
                        i += 2;
                    } else dec.push_back(raw[i]);
                }
                return dec;
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return "";
    };

    auto send_all = [](SockT s, const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            auto n = send(s, reinterpret_cast<const char*>(data + sent),
#ifdef _WIN32
                          static_cast<int>(len - sent),
#else
                          len - sent,
#endif
                          0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    };

    while (true) {
        sockaddr_in cli{};
#ifdef _WIN32
        int clen = sizeof(cli);
#else
        socklen_t clen = sizeof(cli);
#endif
        SockT c = accept(srv, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c < 0) continue;

        char req_buf[4096] = {0};
        auto got = recv(c, req_buf, sizeof(req_buf) - 1, 0);
        if (got <= 0) { close_sock(c); continue; }

        std::string req_line(req_buf);
        size_t sp1 = req_line.find(' ');
        size_t sp2 = req_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
        std::string target = (sp1 != std::string::npos && sp2 != std::string::npos)
                                 ? req_line.substr(sp1 + 1, sp2 - sp1 - 1)
                                 : "/";
        std::string path = target;
        std::string query;
        size_t qm = target.find('?');
        if (qm != std::string::npos) {
            path = target.substr(0, qm);
            query = target.substr(qm + 1);
        }

        if (path == "/" || path == "/index.html") {
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: "
                << VIEWER_SHELL.size() << "\r\nConnection: close\r\n\r\n";
            std::string h = hdr.str();
            send_all(c, reinterpret_cast<const uint8_t*>(h.data()), h.size());
            send_all(c, reinterpret_cast<const uint8_t*>(VIEWER_SHELL.data()), VIEWER_SHELL.size());
        } else if (path == "/api/frame.png" || path == "/api/event") {
            std::vector<uint8_t> png;
            bool hand = false;
            double ms = 0.0;
            {
                std::lock_guard<std::mutex> lk(eng_mtx);
                if (path == "/api/event") {
                    std::string tp = query_param(query, "type");
                    float x = std::strtof(query_param(query, "x").c_str(), nullptr);
                    float y = std::strtof(query_param(query, "y").c_str(), nullptr);
                    if (tp == "click") eng.on_mouse_click(x, y);
                    else if (tp == "move") hand = eng.on_mouse_move(x, y);
                    else if (tp == "wheel") eng.on_mouse_wheel(x, y, std::strtof(query_param(query, "dy").c_str(), nullptr));
                    else if (tp == "key") {
                        std::string k = query_param(query, "k");
                        eng.on_key_input(k, k.size() == 1);
                    }
                }
                eng.render_if_needed();
                ms = eng.last_paint_ms;
                png = eng.painter.encode_png();
            }
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: no-store\r\n"
                << "X-Helios-Paint-Ms: " << std::fixed << std::setprecision(2) << ms << "\r\n"
                << "X-Helios-Cursor: " << (hand ? "pointer" : "arrow") << "\r\n"
                << "Content-Length: " << png.size() << "\r\nConnection: close\r\n\r\n";
            std::string h = hdr.str();
            send_all(c, reinterpret_cast<const uint8_t*>(h.data()), h.size());
            send_all(c, png.data(), png.size());
        } else if (path.rfind("/download/", 0) == 0) {
            std::string fname = path.substr(10);
            if (fname == "minecc-studio-windows-x86_64.exe" ||
                fname == "minecc-studio-linux-x86_64" ||
                fname == "minecc-uic-windows-x86_64.exe" ||
                fname == "minecc-uic-linux-x86_64" ||
                fname == "mcsrc.muib")
            {
                std::filesystem::path fpath = std::filesystem::path(bin_dir) / fname;
                std::ifstream f(fpath, std::ios::binary | std::ios::ate);
                if (f) {
                    auto sz = f.tellg();
                    f.seekg(0);
                    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
                    f.read(reinterpret_cast<char*>(bytes.data()), sz);
                    std::ostringstream hdr;
                    hdr << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        << "Content-Disposition: attachment; filename=\"" << fname << "\"\r\n"
                        << "Content-Length: " << bytes.size() << "\r\nConnection: close\r\n\r\n";
                    std::string h = hdr.str();
                    send_all(c, reinterpret_cast<const uint8_t*>(h.data()), h.size());
                    send_all(c, bytes.data(), bytes.size());
                    close_sock(c);
                    continue;
                }
            }
            const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
            send_all(c, reinterpret_cast<const uint8_t*>(nf), std::strlen(nf));
        } else {
            const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
            send_all(c, reinterpret_cast<const uint8_t*>(nf), std::strlen(nf));
        }
        close_sock(c);
    }
    return 0;
}

} // namespace helios
