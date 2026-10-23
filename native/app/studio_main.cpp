// minecc v2.0 — Native C++20 Self-Rendered Dark Studio + Headless Ghidra-Grade AI CLI
// Zero WebView · Zero Browser Engine · Zero Proprietary Mojang Code Distributed

#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "native/include/flux/ai_cli.hpp"
#include "native/include/flux/platform_io.hpp"

namespace {

bool is_ai_subcommand(const std::string& s) {
    return s == "ai" || s == "skill" || s == "overview" || s == "tree" ||
           s == "search" || s == "inspect" || s == "decompile" ||
           s == "disasm" || s == "xrefs" || s == "callgraph" || s == "strings";
}

void print_help() {
    std::cout
        << "minecc v2.0.0 — Pure C++20 Self-Rendered Java & Minecraft Studio + Headless AI CLI\n\n"
        << "GUI & Preview Modes:\n"
        << "  minecc                              Launch native dark desktop studio (Home Screen)\n"
        << "  minecc <path/to/26.3.jar>           Launch studio pre-loaded with local JAR\n"
        << "  minecc --serve <port>               Run live interactive HTML5 canvas server on 0.0.0.0:<port>\n"
        << "  minecc --render-png <out.png> [--screen home|code|disasm|xrefs|strings|palette] [--class <Name>]\n\n"
        << "Headless AI Reverse-Engineering CLI (Zero MCP / Zero JVM Overhead):\n"
        << "  minecc ai skill                     Print full AI Agent playbook & command reference\n"
        << "  minecc ai overview [--jar <jar>] [--json]\n"
        << "  minecc ai tree [pkg] [--depth N] [--jar <jar>] [--json]\n"
        << "  minecc ai search <query> [--scope all|classes|methods|fields|strings] [--json]\n"
        << "  minecc ai inspect <Class> [--jar <jar>] [--json]\n"
        << "  minecc ai decompile <Class> [--lines A-B] [--jar <jar>] [--json]\n"
        << "  minecc ai disasm <Class> [method] [--jar <jar>] [--json]\n"
        << "  minecc ai xrefs <Symbol> [--jar <jar>] [--json]\n"
        << "  minecc ai callgraph <Class> [--jar <jar>] [--json]\n"
        << "  minecc ai strings [query] [--limit N] [--jar <jar>] [--json]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2) {
            std::string first = argv[1];
            if (first == "--version" || first == "-v") {
                flux::platform::ensure_cli_streams_ready();
                std::cout << "minecc 2.0.0 (FluxUI Pure C++20 Self-Renderer + Headless AI CLI)\n";
                return 0;
            }
            if (first == "--help" || first == "-h" || first == "help") {
                flux::platform::ensure_cli_streams_ready();
                print_help();
                return 0;
            }
            // Route BOTH `minecc ai <subcmd> ...` AND direct shorthand `minecc <subcmd> ...` cleanly!
            if (is_ai_subcommand(first)) {
                flux::platform::ensure_cli_streams_ready();
                std::vector<std::string> ai_args;
                int start_i = (first == "ai") ? 2 : 1;
                for (int i = start_i; i < argc; ++i) ai_args.emplace_back(argv[i]);
                return flux::ai::run_ai_cli(ai_args);
            }
        }

        std::string jar_path;
        std::string render_png;
        std::string screen_name = "home";
        std::string initial_class;
        std::string palette_seed;
        int serve_port = 0;
        int win_w = 1440;
        int win_h = 880;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--jar" && i + 1 < argc) jar_path = argv[++i];
            else if (a == "--render-png" && i + 1 < argc) render_png = argv[++i];
            else if (a == "--screen" && i + 1 < argc) screen_name = argv[++i];
            else if (a == "--class" && i + 1 < argc) initial_class = argv[++i];
            else if (a == "--query" && i + 1 < argc) palette_seed = argv[++i];
            else if (a == "--serve" && i + 1 < argc) serve_port = std::atoi(argv[++i]);
            else if (a == "--width" && i + 1 < argc) win_w = std::atoi(argv[++i]);
            else if (a == "--height" && i + 1 < argc) win_h = std::atoi(argv[++i]);
            else if (a.size() > 4 && a.substr(a.size() - 4) == ".jar") jar_path = a;
        }

        flux::StudioApp app;
        if (jar_path.empty()) jar_path = flux::jvm::BinaryWorkspace::detect_local_minecraft_jar();
        if (!jar_path.empty()) {
            app.load_jar_file(jar_path, /*sync_index=*/!render_png.empty());
        }

        if (!initial_class.empty()) app.open_class(initial_class);

        if (screen_name == "home") app.switch_screen(flux::Screen::Home);
        else if (screen_name == "code") app.switch_screen(flux::Screen::Code);
        else if (screen_name == "disasm") app.switch_screen(flux::Screen::Disasm);
        else if (screen_name == "xrefs") app.switch_screen(flux::Screen::XRefs);
        else if (screen_name == "strings") app.switch_screen(flux::Screen::Strings);
        else if (screen_name == "palette") {
            app.switch_screen(flux::Screen::Code);
            app.toggle_palette(true, palette_seed);
            app.palette_anim_t = 1.0f;
        }

        // Settle initial view spring for deterministic headless screenshot output
        app.view_enter_t = 1.0f;
        app.rail_pill_y = flux::StudioApp::rail_slot_y(app.screen);

        if (!render_png.empty()) {
            flux::platform::ensure_cli_streams_ready();
            flux::FluxCanvas cv;
            cv.resize(win_w, win_h);
            app.render(cv);
            if (!cv.save_png(render_png)) {
                std::cerr << "[minecc] Failed to write PNG: " << render_png << "\n";
                return 2;
            }
            std::cout << "[minecc] Rendered " << cv.width << "x" << cv.height
                      << " (" << screen_name << ") in " << app.last_render_ms << " ms -> " << render_png << "\n";
            return 0;
        }

        if (serve_port > 0) {
            flux::platform::ensure_cli_streams_ready();
            return flux::platform::run_preview_server(app, serve_port);
        }

        return flux::platform::run_desktop_window(app, win_w, win_h);
    } catch (const std::exception& ex) {
        flux::platform::ensure_cli_streams_ready();
        std::cerr << "[minecc fatal guard] " << ex.what() << "\n";
        return 3;
    } catch (...) {
        return 4;
    }
}
