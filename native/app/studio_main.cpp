// studio_main.cpp — Single Unified Entrypoint for `minecc` v2.1.0 (Windows .exe & Linux ELF)
//   1. `minecc`                         -> Hardware GPU OpenGL Desktop UI + Startup Auto-Updater
//   2. `minecc ai <subcommand> [...]`   -> Pure C++20 Headless AI CLI (decompile, diff, export-all, disasm, xrefs)
//   3. `minecc --serve <port>`          -> Serves 60/120fps WebAssembly Studio (`docs/*`) on `0.0.0.0:<port>`
//   4. `minecc --benchmark-fps`         -> Measures native UI CPU frame build speed (`< 0.1 ms`!)

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "native/include/flux/ai_cli.hpp"
#include "native/include/flux/platform_io.hpp"

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "ai") {
        return flux::run_ai_cli(argc, argv);
    }

    std::string jar_arg, base_jar_arg;
    int serve_port = 0;
    bool simulate_update = false;
    bool benchmark_fps = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "minecc 2.1.0 (Pure C++20 Native Java 8–25 Decompiler, Diff & GPU UI)\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::cout
                << "minecc 2.1.0 — Clean-Room Native C++20 Java Decompiler, Version Diff & Studio\n\n"
                << "Usage:\n"
                << "  minecc                              Launch native desktop studio\n"
                << "  minecc --jar <path.jar>             Launch with target JAR loaded\n"
                << "  minecc --base-jar <path.jar>        Preload base JAR for Version Diff\n"
                << "  minecc --simulate-update            Test startup auto-update notification banner\n"
                << "  minecc --serve <port>               Run live WebAssembly Studio preview on 0.0.0.0:<port>\n"
                << "  minecc ai --help                    Headless C++20 CLI commands\n";
            return 0;
        }
        if (a == "--jar" && i + 1 < argc) jar_arg = argv[++i];
        else if (a == "--base-jar" && i + 1 < argc) base_jar_arg = argv[++i];
        else if (a == "--serve" && i + 1 < argc) serve_port = std::stoi(argv[++i]);
        else if (a == "--simulate-update") simulate_update = true;
        else if (a == "--benchmark-fps") benchmark_fps = true;
        else if (!a.empty() && a[0] != '-' && jar_arg.empty() && std::filesystem::is_regular_file(a)) {
            jar_arg = a;
        }
    }

    flux::StudioApp app;
    // Automatically run non-blocking GitHub Release update check on every startup!
    app.auto_updater.start_async_check(argc > 0 ? argv[0] : "", simulate_update);

    if (!base_jar_arg.empty()) app.load_jar_file(base_jar_arg, true, false);
    if (!jar_arg.empty()) app.load_jar_file(jar_arg, false, false);

    if (benchmark_fps) {
        flux::FluxCanvas cv;
        cv.width = 1920; cv.height = 1080;
        auto t0 = std::chrono::steady_clock::now();
        constexpr int FRAMES = 500;
        for (int f = 0; f < FRAMES; ++f) {
            app.tick_animations(1.0f / 120.0f);
            app.render(cv);
        }
        double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "[benchmark] 1920x1080 native UI batch: " << (total_ms / FRAMES)
                  << " ms/frame (" << int(1000.0 / (total_ms / FRAMES)) << " theoretical FPS, "
                  << cv.verts.size() / 6 << " GPU quads/frame)\n";
        return 0;
    }

    if (serve_port > 0) {
        return flux::run_web_preview_server(app, serve_port);
    }

    return flux::run_native_window(app);
}
