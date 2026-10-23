#pragma once
// FluxPlatformIO: Bulletproof Cross-Platform Native Windowing (Win32 Dark Mode + X11)
//                 + Pipe-Safe Headless Console Bridge + Live Interactive Preview Server

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "native/include/flux/studio_ui.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <windowsx.h>
#  include <commdlg.h>
#  include <dwmapi.h>
#else
#  include <arpa/inet.h>
#  include <dlfcn.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace flux::platform {

// Ensures CLI mode works BOTH when piped by AI coding agents (`subprocess.run` / `$(minecc ai ...)`)
// AND when invoked interactively in Windows Terminal / PowerShell despite `-mwindows` subsystem!
inline void ensure_cli_streams_ready() noexcept {
#if defined(_WIN32)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    bool pipe_or_file_valid = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN);
    if (!pipe_or_file_valid) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
            std::ios::sync_with_stdio(true);
        }
    }
#endif
}

inline std::string pick_jar_dialog() {
#if defined(_WIN32)
    char file_buf[1024] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = sizeof(file_buf);
    ofn.lpstrFilter = "Java / Minecraft Archives (*.jar)\0*.jar\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        return std::string(file_buf);
    }
    return {};
#else
    return jvm::BinaryWorkspace::detect_local_minecraft_jar();
#endif
}

#if defined(_WIN32)
namespace win32_detail {
struct WinCtx {
    StudioApp* app = nullptr;
    FluxCanvas canvas;
    bool dirty = true;
    std::chrono::steady_clock::time_point last_tick = std::chrono::steady_clock::now();
};

inline LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    auto* ctx = reinterpret_cast<WinCtx*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    if (!ctx || !ctx->app) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // Eliminate flicker completely!
        case WM_SIZE: {
            int w = std::max(640, int(LOWORD(lp)));
            int h = std::max(420, int(HIWORD(lp)));
            ctx->canvas.resize(w, h);
            ctx->dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (ctx->app->on_mouse_move(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)))) {
                ctx->dirty = true; // Coalesced onto the 60 Hz timer — ZERO high-DPI gaming mouse lag!
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (ctx->app->on_click(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)))) {
                ctx->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (ctx->app->open_jar_requested) {
                ctx->app->open_jar_requested = false;
                std::string picked = pick_jar_dialog();
                if (!picked.empty()) ctx->app->load_jar_file(picked, false);
                ctx->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float notches = float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
            if (ctx->app->on_scroll(float(pt.x), float(pt.y), notches)) ctx->dirty = true;
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wp == VK_UP)   { if (ctx->app->on_key(1001, false)) ctx->dirty = true; }
            if (wp == VK_DOWN) { if (ctx->app->on_key(1002, false)) ctx->dirty = true; }
            if (wp == VK_ESCAPE) { if (ctx->app->on_key(27, false)) ctx->dirty = true; }
            if (ctrl && (wp == 'K' || wp == 'P' || wp == 'O')) {
                if (ctx->app->on_key(int(wp), true)) ctx->dirty = true;
                if (ctx->app->open_jar_requested) {
                    ctx->app->open_jar_requested = false;
                    std::string picked = pick_jar_dialog();
                    if (!picked.empty()) ctx->app->load_jar_file(picked, false);
                    ctx->dirty = true;
                }
            }
            return 0;
        }
        case WM_CHAR: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!ctrl && wp >= 8 && wp < 127) {
                if (ctx->app->on_key(int(wp), false)) ctx->dirty = true;
            }
            return 0;
        }
        case WM_TIMER: {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - ctx->last_tick).count();
            ctx->last_tick = now;
            if (ctx->app->tick_animations(dt)) ctx->dirty = true;
            if (ctx->dirty) {
                ctx->dirty = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            ctx->app->render(ctx->canvas);
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = ctx->canvas.width;
            bmi.bmiHeader.biHeight = -ctx->canvas.height; // top-down scanline order!
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, ctx->canvas.width, ctx->canvas.height,
                          0, 0, ctx->canvas.width, ctx->canvas.height,
                          ctx->canvas.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
} // namespace win32_detail
#endif

// =========================================================================
// LIVE INTERACTIVE HTTP PREVIEW SERVER (`0.0.0.0:<port>`)
// =========================================================================
inline int run_preview_server(StudioApp& app, int port) {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return 1;
    int opt = 1;
#if defined(_WIN32)
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0 required for live preview!
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(srv, 64) != 0) {
        std::cerr << "[minecc] Cannot bind 0.0.0.0:" << port << "\n";
        return 1;
    }

    FluxCanvas shared_cv;
    shared_cv.resize(1440, 880);
    std::mutex mtx;
    std::vector<uint8_t> png_cache;
    {
        app.tick_animations(0.05f);
        app.render(shared_cv);
        png_cache = shared_cv.encode_png();
    }

    std::atomic<bool> alive{true};
    std::thread anim_worker([&]() {
        auto prev = std::chrono::steady_clock::now();
        while (alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            prev = now;
            std::lock_guard<std::mutex> lk(mtx);
            if (app.tick_animations(dt)) {
                app.render(shared_cv);
                png_cache = shared_cv.encode_png();
            }
        }
    });

    std::cout << "[minecc-studio] Live Interactive Self-Renderer Preview listening on http://0.0.0.0:" << port << std::endl;

    static const std::string SHELL_HTML = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>minecc v2.0 — Native C++20 Studio</title>
<style>
  html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:#07090d;display:flex;align-items:center;justify-content:center;user-select:none}
  canvas{max-width:100vw;max-height:100vh;cursor:pointer;display:block}
</style></head><body>
<canvas id="c" width="1440" height="880"></canvas>
<script>
const cv=document.getElementById('c'),ctx=cv.getContext('2d');
let busy=false;
async function pull(){
  if(busy)return; busy=true;
  try{
    const r=await fetch('/frame.png?t='+Date.now(),{cache:'no-store'});
    const b=await createImageBitmap(await r.blob());
    if(cv.width!==b.width||cv.height!==b.height){cv.width=b.width;cv.height=b.height;}
    ctx.drawImage(b,0,0); b.close();
  }catch(_){}
  busy=false;
}
function mapXY(e){
  const r=cv.getBoundingClientRect();
  return [Math.round((e.clientX-r.left)*cv.width/r.width), Math.round((e.clientY-r.top)*cv.height/r.height)];
}
let mvTimer=null;
cv.addEventListener('mousemove',e=>{
  const [x,y]=mapXY(e);
  clearTimeout(mvTimer);
  mvTimer=setTimeout(()=>fetch(`/ev?t=move&x=${x}&y=${y}`).then(pull),18);
});
cv.addEventListener('mousedown',async e=>{
  const [x,y]=mapXY(e);
  await fetch(`/ev?t=click&x=${x}&y=${y}`); pull();
});
cv.addEventListener('wheel',async e=>{
  e.preventDefault();
  const [x,y]=mapXY(e);
  await fetch(`/ev?t=wheel&x=${x}&y=${y}&d=${e.deltaY>0?-1.5:1.5}`); pull();
},{passive:false});
window.addEventListener('keydown',async e=>{
  if((e.ctrlKey||e.metaKey)&&(e.key==='k'||e.key==='K')){e.preventDefault();await fetch('/ev?t=key&k=75&c=1');pull();return;}
  if(e.key==='Escape'){await fetch('/ev?t=key&k=27&c=0');pull();return;}
  if(e.key==='Backspace'){await fetch('/ev?t=key&k=8&c=0');pull();return;}
  if(e.key==='Enter'){await fetch('/ev?t=key&k=13&c=0');pull();return;}
  if(e.key==='ArrowUp'){await fetch('/ev?t=key&k=1001&c=0');pull();return;}
  if(e.key==='ArrowDown'){await fetch('/ev?t=key&k=1002&c=0');pull();return;}
  if(e.key.length===1&&!e.ctrlKey&&!e.metaKey){await fetch(`/ev?t=key&k=${e.key.charCodeAt(0)}&c=0`);pull();}
});
pull(); setInterval(pull, 90);
</script></body></html>)HTML";

    auto qparam = [](const std::string& q, const std::string& k) -> float {
        size_t p = q.find(k + "=");
        if (p == std::string::npos) return 0.0f;
        return std::strtof(q.c_str() + p + k.size() + 1, nullptr);
    };

    while (true) {
        int cli = ::accept(srv, nullptr, nullptr);
        if (cli < 0) continue;
        char buf[2048]{};
#if defined(_WIN32)
        int n = ::recv(cli, buf, sizeof(buf) - 1, 0);
#else
        ssize_t n = ::read(cli, buf, sizeof(buf) - 1);
#endif
        if (n > 0) {
            std::string req(buf, size_t(n));
            size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            std::string url = (sp1 != std::string::npos && sp2 != std::string::npos) ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "/";

            if (url.rfind("/ev?", 0) == 0) {
                std::lock_guard<std::mutex> lk(mtx);
                bool chg = false;
                if (url.find("t=move") != std::string::npos) chg = app.on_mouse_move(qparam(url, "x"), qparam(url, "y"));
                else if (url.find("t=click") != std::string::npos) chg = app.on_click(qparam(url, "x"), qparam(url, "y"));
                else if (url.find("t=wheel") != std::string::npos) chg = app.on_scroll(qparam(url, "x"), qparam(url, "y"), qparam(url, "d"));
                else if (url.find("t=key") != std::string::npos) chg = app.on_key(int(qparam(url, "k")), qparam(url, "c") > 0.5f);
                if (chg) {
                    app.tick_animations(0.016f);
                    app.render(shared_cv);
                    png_cache = shared_cv.encode_png();
                }
                std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
#if defined(_WIN32)
                ::send(cli, hdr.data(), int(hdr.size()), 0);
#else
                if (::write(cli, hdr.data(), hdr.size()) < 0) {}
#endif
            } else if (url.rfind("/frame.png", 0) == 0) {
                std::vector<uint8_t> copy;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    copy = png_cache;
                }
                std::ostringstream hs;
                hs << "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: no-store\r\nContent-Length: "
                   << copy.size() << "\r\nConnection: close\r\n\r\n";
                std::string hdr = hs.str();
#if defined(_WIN32)
                ::send(cli, hdr.data(), int(hdr.size()), 0);
                ::send(cli, reinterpret_cast<const char*>(copy.data()), int(copy.size()), 0);
#else
                if (::write(cli, hdr.data(), hdr.size()) < 0) {}
                if (::write(cli, copy.data(), copy.size()) < 0) {}
#endif
            } else {
                std::ostringstream hs;
                hs << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                   << SHELL_HTML.size() << "\r\nConnection: close\r\n\r\n";
                std::string hdr = hs.str();
#if defined(_WIN32)
                ::send(cli, hdr.data(), int(hdr.size()), 0);
                ::send(cli, SHELL_HTML.data(), int(SHELL_HTML.size()), 0);
#else
                if (::write(cli, hdr.data(), hdr.size()) < 0) {}
                if (::write(cli, SHELL_HTML.data(), SHELL_HTML.size()) < 0) {}
#endif
            }
        }
#if defined(_WIN32)
        closesocket(cli);
#else
        ::close(cli);
#endif
    }
    alive.store(false);
    anim_worker.join();
    return 0;
}

// =========================================================================
// NATIVE DESKTOP WINDOW RUNNER (Win32 Immersive Dark Mode + X11 Dynamic Load)
// =========================================================================
inline int run_desktop_window(StudioApp& app, int init_w, int init_h) {
#if defined(_WIN32)
    SetProcessDPIAware();
    win32_detail::WinCtx ctx{};
    ctx.app = &app;
    ctx.canvas.resize(init_w, init_h);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = win32_detail::wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "MineccFluxNativeWindow";
    RegisterClassExA(&wc);

    RECT wr{0, 0, init_w, init_h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "minecc v2.0 — Native C++20 Java & Minecraft Studio",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, &ctx);
    if (!hwnd) return 1;

    // Enable native Windows 11 Dark Titlebar (`DWMWA_USE_IMMERSIVE_DARK_MODE = 20`)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    // Smooth 60 Hz animation & hover frame coalescer (`0.0%` CPU when settled!)
    SetTimer(hwnd, 1, 16, nullptr);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
#else
    // Dynamic `dlopen("libX11.so.6")` so headless servers / CI containers NEVER fail missing X11!
    void* xlib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!xlib) return run_preview_server(app, 7890);

    using XOpenDisplay_t = void* (*)(const char*);
    using XDefaultScreen_t = int (*)(void*);
    using XRootWindow_t = unsigned long (*)(void*, int);
    using XDefaultVisual_t = void* (*)(void*, int);
    using XDefaultDepth_t = int (*)(void*, int);
    using XDefaultGC_t = void* (*)(void*, int);
    using XCreateSimpleWindow_t = unsigned long (*)(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
    using XSelectInput_t = int (*)(void*, unsigned long, long);
    using XMapWindow_t = int (*)(void*, unsigned long);
    using XStoreName_t = int (*)(void*, unsigned long, const char*);
    using XPending_t = int (*)(void*);
    using XNextEvent_t = int (*)(void*, void*);
    using XCreateImage_t = void* (*)(void*, void*, unsigned int, int, int, char*, unsigned int, unsigned int, int, int);
    using XPutImage_t = int (*)(void*, unsigned long, void*, void*, int, int, int, int, unsigned int, unsigned int);
    using XConnectionNumber_t = int (*)(void*);
    using XLookupKeysym_t = unsigned long (*)(void*, int);
    using XInternAtom_t = unsigned long (*)(void*, const char*, int);
    using XSetWMProtocols_t = int (*)(void*, unsigned long, unsigned long*, int);
    using XCloseDisplay_t = int (*)(void*);

    auto pOpen = reinterpret_cast<XOpenDisplay_t>(dlsym(xlib, "XOpenDisplay"));
    void* dpy = pOpen ? pOpen(nullptr) : nullptr;
    if (!dpy) {
        dlclose(xlib);
        return run_preview_server(app, 7890);
    }

    auto pScreen = reinterpret_cast<XDefaultScreen_t>(dlsym(xlib, "XDefaultScreen"));
    auto pRoot = reinterpret_cast<XRootWindow_t>(dlsym(xlib, "XRootWindow"));
    auto pVis = reinterpret_cast<XDefaultVisual_t>(dlsym(xlib, "XDefaultVisual"));
    auto pDepth = reinterpret_cast<XDefaultDepth_t>(dlsym(xlib, "XDefaultDepth"));
    auto pGC = reinterpret_cast<XDefaultGC_t>(dlsym(xlib, "XDefaultGC"));
    auto pCreate = reinterpret_cast<XCreateSimpleWindow_t>(dlsym(xlib, "XCreateSimpleWindow"));
    auto pSelect = reinterpret_cast<XSelectInput_t>(dlsym(xlib, "XSelectInput"));
    auto pMap = reinterpret_cast<XMapWindow_t>(dlsym(xlib, "XMapWindow"));
    auto pStore = reinterpret_cast<XStoreName_t>(dlsym(xlib, "XStoreName"));
    auto pPending = reinterpret_cast<XPending_t>(dlsym(xlib, "XPending"));
    auto pNext = reinterpret_cast<XNextEvent_t>(dlsym(xlib, "XNextEvent"));
    auto pCreateImg = reinterpret_cast<XCreateImage_t>(dlsym(xlib, "XCreateImage"));
    auto pPutImg = reinterpret_cast<XPutImage_t>(dlsym(xlib, "XPutImage"));
    auto pConn = reinterpret_cast<XConnectionNumber_t>(dlsym(xlib, "XConnectionNumber"));
    auto pKeysym = reinterpret_cast<XLookupKeysym_t>(dlsym(xlib, "XLookupKeysym"));
    auto pAtom = reinterpret_cast<XInternAtom_t>(dlsym(xlib, "XInternAtom"));
    auto pSetProto = reinterpret_cast<XSetWMProtocols_t>(dlsym(xlib, "XSetWMProtocols"));
    auto pClose = reinterpret_cast<XCloseDisplay_t>(dlsym(xlib, "XCloseDisplay"));

    int scr = pScreen(dpy);
    unsigned long win = pCreate(dpy, pRoot(dpy, scr), 40, 40, init_w, init_h, 0, 0x07090Du, 0x07090Du);
    pStore(dpy, win, "minecc v2.0 — Native C++20 Java & Minecraft Studio");
    pSelect(dpy, win, (1L << 15) | (1L << 2) | (1L << 6) | (1L << 0) | (1L << 17));
    unsigned long wm_del = pAtom(dpy, "WM_DELETE_WINDOW", 0);
    pSetProto(dpy, win, &wm_del, 1);
    pMap(dpy, win);

    FluxCanvas cv;
    cv.resize(init_w, init_h);
    bool dirty = true;
    bool running = true;
    int xfd = pConn(dpy);
    auto last_t = std::chrono::steady_clock::now();

    struct RawXEv {
        int type; unsigned long serial; int send_event; void* display; unsigned long window;
        unsigned long root; unsigned long subwindow; unsigned long time;
        int x, y, x_root, y_root; unsigned int state, button_or_key; char pad[192];
    };

    while (running) {
        while (pPending(dpy) > 0) {
            RawXEv ev{};
            pNext(dpy, &ev);
            if (ev.type == 12) dirty = true; // Expose
            else if (ev.type == 22) {        // ConfigureNotify
                int* ip = reinterpret_cast<int*>(&ev);
                int nw = std::max(640, ip[14]), nh = std::max(420, ip[15]);
                if (nw != cv.width || nh != cv.height) { cv.resize(nw, nh); dirty = true; }
            } else if (ev.type == 6) {       // MotionNotify (coalesced!)
                if (app.on_mouse_move(float(ev.x), float(ev.y))) dirty = true;
            } else if (ev.type == 4) {       // ButtonPress
                if (ev.button_or_key == 1) { if (app.on_click(float(ev.x), float(ev.y))) dirty = true; }
                else if (ev.button_or_key == 4) { if (app.on_scroll(float(ev.x), float(ev.y), 1.2f)) dirty = true; }
                else if (ev.button_or_key == 5) { if (app.on_scroll(float(ev.x), float(ev.y), -1.2f)) dirty = true; }
            } else if (ev.type == 2) {       // KeyPress
                bool ctrl = (ev.state & 4u) != 0u;
                unsigned long ks = pKeysym(&ev, (ev.state & 1u) ? 1 : 0);
                if (ks == 0xFF1B) { if (app.on_key(27, false)) dirty = true; }
                else if (ks == 0xFF08) { if (app.on_key(8, false)) dirty = true; }
                else if (ks == 0xFF0D) { if (app.on_key(13, false)) dirty = true; }
                else if (ks == 0xFF52) { if (app.on_key(1001, false)) dirty = true; }
                else if (ks == 0xFF54) { if (app.on_key(1002, false)) dirty = true; }
                else if (ks >= 32 && ks < 127) { if (app.on_key(int(ks), ctrl)) dirty = true; }
            } else if (ev.type == 33) {      // ClientMessage
                running = false;
            }
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_t).count();
        last_t = now;
        bool animating = app.tick_animations(dt);
        if (animating) dirty = true;

        if (dirty) {
            dirty = false;
            app.render(cv);
            void* ximg = pCreateImg(dpy, pVis(dpy, scr), pDepth(dpy, scr), 2, 0,
                                    reinterpret_cast<char*>(cv.pixels.data()), cv.width, cv.height, 32, cv.width * 4);
            if (ximg) {
                pPutImg(dpy, win, pGC(dpy, scr), ximg, 0, 0, 0, 0, cv.width, cv.height);
                *reinterpret_cast<char**>(reinterpret_cast<char*>(ximg) + sizeof(void*) * 2 + sizeof(int) * 4) = nullptr;
            }
        }

        pollfd pfd{xfd, POLLIN, 0};
        // 16ms (60 FPS) ONLY while springs/indexing move; literal 0.00% CPU block when settled!
        ::poll(&pfd, 1, animating ? 16 : -1);
    }

    pClose(dpy);
    dlclose(xlib);
    return 0;
#endif
}

} // namespace flux::platform
