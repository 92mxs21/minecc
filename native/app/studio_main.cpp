// minecc-studio: Native C++20 Minecraft 26.3 Code & Bytecode Explorer (Helios HTML5+CSS3+ES6 Software Engine)
// Zero WebView2, Zero Chromium, Zero Electron, Zero proprietary Mojang assets embedded in binary.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "native/generated/mcsrc_bundle.hpp"
#include "native/include/helios/bundle_format.hpp"
#include "native/include/helios/jvm_reader.hpp"
#include "native/include/helios/platform.hpp"

namespace fs = std::filesystem;

namespace {

bool try_load_self_trailer_bundle(const std::string& exe_path, helios::bundle::BundlePayload& out) {
    std::ifstream f(exe_path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    if (sz < 32) return false;
    size_t total = static_cast<size_t>(sz);
    uint8_t tail[16] = {0};
    f.seekg(static_cast<std::streamoff>(total - 16));
    f.read(reinterpret_cast<char*>(tail), 16);
    if (std::memcmp(tail + 8, "HELIOSEX", 8) != 0) return false;

    size_t p = 0;
    uint64_t muib_len = helios::bundle::read_u64_le(tail, 16, p);
    if (muib_len == 0 || muib_len + 16 > total) return false;

    std::vector<uint8_t> buf(static_cast<size_t>(muib_len));
    f.seekg(static_cast<std::streamoff>(total - 16 - muib_len));
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(muib_len));
    out = helios::bundle::deserialize_bundle(buf.data(), buf.size());
    return true;
}

void bind_workspace_host_api(helios::HeliosEngine& engine, helios::jvm::CodeWorkspace& ws) {
    using namespace helios::js;
    auto host = make_object();

    host.obj->props["listClasses"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
        std::vector<JsValue> list;
        list.reserve(ws.classes.size());
        for (const auto& [name, rec] : ws.classes) {
            auto o = make_object();
            o.obj->props["name"] = JsValue::string(name);
            o.obj->props["kind"] = JsValue::string(rec.kind);
            list.push_back(std::move(o));
        }
        return make_array(std::move(list));
    });

    host.obj->props["hasClass"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
        if (a.empty()) return JsValue::boolean(false);
        return JsValue::boolean(ws.classes.find(a[0].to_str()) != ws.classes.end());
    });

    host.obj->props["getStats"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
        auto o = make_object();
        o.obj->props["mode"] = JsValue::string(ws.active_mode);
        o.obj->props["loadedCount"] = JsValue::number(static_cast<double>(ws.classes.size()));
        return o;
    });

    host.obj->props["inspectClass"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
        std::string cls = a.empty() ? "net/minecraft/DetectedVersion" : a[0].to_str();
        std::string ver = a.size() >= 2 ? a[1].to_str() : "26.3";

        auto o = make_object();
        auto it = ws.classes.find(cls);
        if (it == ws.classes.end()) {
            o.obj->props["source"] = JsValue::string("// Class not found: " + cls);
            o.obj->props["bytecode"] = JsValue::string("// No bytecode available");
            o.obj->props["diff"] = JsValue::string("// No diff available");
            o.obj->props["kind"] = JsValue::string("class");
            o.obj->props["flagsHex"] = JsValue::string("0001");
            o.obj->props["superName"] = JsValue::string("java/lang/Object");
            o.obj->props["interfaces"] = make_array();
            o.obj->props["subtypes"] = make_array();
            o.obj->props["methods"] = make_array();
            o.obj->props["diffAdded"] = JsValue::number(0);
            o.obj->props["diffRemoved"] = JsValue::number(0);
            return o;
        }

        const auto& rec = it->second;
        std::ostringstream fx;
        fx << std::hex << rec.access_flags;

        std::vector<JsValue> itfs, subs, mtds;
        for (const auto& i : rec.interfaces) itfs.push_back(JsValue::string(i));
        for (const auto& m : rec.methods) mtds.push_back(JsValue::string(m));
        for (const auto& [other_name, other_rec] : ws.classes) {
            if (other_rec.super_name == cls ||
                std::find(other_rec.interfaces.begin(), other_rec.interfaces.end(), cls) != other_rec.interfaces.end())
            {
                subs.push_back(JsValue::string(other_name));
            }
        }

        o.obj->props["source"] = JsValue::string(ws.read_java_source(cls, ver));
        o.obj->props["bytecode"] = JsValue::string(rec.bytecode_26_3);
        o.obj->props["diff"] = JsValue::string(rec.diff_26_4);
        o.obj->props["kind"] = JsValue::string(rec.kind);
        o.obj->props["flagsHex"] = JsValue::string(fx.str());
        o.obj->props["superName"] = JsValue::string(rec.super_name);
        o.obj->props["interfaces"] = make_array(std::move(itfs));
        o.obj->props["subtypes"] = make_array(std::move(subs));
        o.obj->props["methods"] = make_array(std::move(mtds));
        o.obj->props["diffAdded"] = JsValue::number(rec.diff_added);
        o.obj->props["diffRemoved"] = JsValue::number(rec.diff_removed);
        return o;
    });

    host.obj->props["search"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
        std::string q = a.empty() ? "" : helios::detail::to_lower_ascii(a[0].to_str());
        std::string scope = a.size() >= 2 ? a[1].to_str() : "Classes";
        std::vector<JsValue> hits;
        const size_t cap = 150;

        for (const auto& [name, rec] : ws.classes) {
            if (hits.size() >= cap) break;
            if (scope == "Classes") {
                if (helios::detail::to_lower_ascii(name).find(q) != std::string::npos) {
                    auto h = make_object();
                    h.obj->props["name"] = JsValue::string(name);
                    h.obj->props["icon"] = JsValue::string(rec.kind);
                    size_t sl = name.rfind('/');
                    h.obj->props["label"] = JsValue::string(sl == std::string::npos ? name : name.substr(sl + 1));
                    h.obj->props["detail"] = JsValue::string(sl == std::string::npos ? "" : name.substr(0, sl));
                    h.obj->props["line"] = JsValue::number(1);
                    hits.push_back(std::move(h));
                }
            } else if (scope == "Methods") {
                for (const auto& m : rec.methods) {
                    if (helios::detail::to_lower_ascii(m).find(q) != std::string::npos) {
                        auto h = make_object();
                        h.obj->props["name"] = JsValue::string(name);
                        h.obj->props["icon"] = JsValue::string("method");
                        h.obj->props["label"] = JsValue::string(m);
                        size_t sl = name.rfind('/');
                        h.obj->props["detail"] = JsValue::string(sl == std::string::npos ? name : name.substr(sl + 1));
                        h.obj->props["line"] = JsValue::number(1);
                        hits.push_back(std::move(h));
                        if (hits.size() >= cap) break;
                    }
                }
            } else if (scope == "Fields") {
                for (const auto& f : rec.fields) {
                    if (helios::detail::to_lower_ascii(f).find(q) != std::string::npos) {
                        auto h = make_object();
                        h.obj->props["name"] = JsValue::string(name);
                        h.obj->props["icon"] = JsValue::string("field");
                        h.obj->props["label"] = JsValue::string(f);
                        size_t sl = name.rfind('/');
                        h.obj->props["detail"] = JsValue::string(sl == std::string::npos ? name : name.substr(sl + 1));
                        h.obj->props["line"] = JsValue::number(1);
                        hits.push_back(std::move(h));
                        if (hits.size() >= cap) break;
                    }
                }
            } else {
                // Content line-by-line search with exact line jump number!
                std::istringstream iss(rec.source_26_3);
                std::string ln_txt;
                int ln_no = 0;
                while (std::getline(iss, ln_txt)) {
                    ++ln_no;
                    if (helios::detail::to_lower_ascii(ln_txt).find(q) != std::string::npos) {
                        auto h = make_object();
                        h.obj->props["name"] = JsValue::string(name);
                        h.obj->props["icon"] = JsValue::string(rec.kind);
                        size_t sl = name.rfind('/');
                        h.obj->props["label"] = JsValue::string((sl == std::string::npos ? name : name.substr(sl + 1)) + ":" + std::to_string(ln_no));
                        h.obj->props["detail"] = JsValue::string(helios::css_detail::trim(ln_txt).substr(0, 28));
                        h.obj->props["line"] = JsValue::number(ln_no);
                        hits.push_back(std::move(h));
                        break;
                    }
                }
            }
        }
        return make_array(std::move(hits));
    });

    host.obj->props["openJarDialog"] = make_native_fn([&ws](JsRuntime&, const JsValue&, const std::vector<JsValue>&) {
        std::string chosen = helios::native_open_jar_dialog();
        if (chosen.empty()) {
            return JsValue::string("Tip: Pass --jar <path/to/26.3.jar> or place 26.3.jar in ~/.minecraft/versions/26.3/26.3.jar");
        }
        std::string msg;
        ws.load_jar_file(chosen, msg);
        return JsValue::string(msg);
    });

    host.obj->props["setClipboard"] = make_native_fn([&engine](JsRuntime&, const JsValue&, const std::vector<JsValue>& a) {
        if (!a.empty()) helios::native_set_clipboard(engine, a[0].to_str());
        return JsValue::undef();
    });

    engine.js_rt->global_env->define("Host", host);
}

} // namespace

int main(int argc, char** argv) {
    std::string custom_bundle_path;
    std::string jar_path;
    std::string png_out;
    std::string serve_addr;
    std::string bin_dir = "bin";
    int win_w = 1380;
    int win_h = 840;
    std::vector<std::pair<float, float>> sim_clicks;
    std::vector<std::string> sim_keys;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bundle" && i + 1 < argc) custom_bundle_path = argv[++i];
        else if (arg == "--jar" && i + 1 < argc) jar_path = argv[++i];
        else if (arg == "--render-png" && i + 1 < argc) png_out = argv[++i];
        else if (arg == "--serve" && i + 1 < argc) serve_addr = argv[++i];
        else if (arg == "--bin-dir" && i + 1 < argc) bin_dir = argv[++i];
        else if (arg == "--size" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t xp = s.find('x');
            if (xp != std::string::npos) {
                win_w = std::atoi(s.substr(0, xp).c_str());
                win_h = std::atoi(s.substr(xp + 1).c_str());
            }
        } else if (arg == "--click" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t cp = s.find(',');
            if (cp != std::string::npos) {
                sim_clicks.emplace_back(std::strtof(s.substr(0, cp).c_str(), nullptr),
                                        std::strtof(s.substr(cp + 1).c_str(), nullptr));
            }
        } else if (arg == "--key" && i + 1 < argc) {
            sim_keys.emplace_back(argv[++i]);
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "minecc-studio 0.2.0 (HeliosUI Native C++20 HTML5+CSS3+ES6 Engine & JVM 69.0 Disassembler)\n";
            return 0;
        }
    }

    try {
        helios::bundle::BundlePayload bundle;
        if (!custom_bundle_path.empty()) {
            std::ifstream f(custom_bundle_path, std::ios::binary | std::ios::ate);
            if (!f) throw std::runtime_error("Cannot open bundle: " + custom_bundle_path);
            auto sz = f.tellg();
            f.seekg(0);
            std::vector<uint8_t> raw(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(raw.data()), sz);
            bundle = helios::bundle::deserialize_bundle(raw.data(), raw.size());
        } else if (argc > 0 && argv[0] && try_load_self_trailer_bundle(argv[0], bundle)) {
            // Packed standalone custom app created via `minecc-uic --pack-exe`!
        } else {
            bundle = helios::bundle::deserialize_bundle(
                helios::embedded::DEFAULT_BUNDLE,
                helios::embedded::DEFAULT_BUNDLE_SIZE);
        }

        helios::jvm::CodeWorkspace workspace;
        if (!jar_path.empty()) {
            std::string msg;
            if (!workspace.load_jar_file(jar_path, msg)) {
                std::cerr << "[minecc-studio] Warning: " << msg << "\n";
            } else {
                std::cout << "[minecc-studio] " << msg << "\n";
            }
        }

        helios::HeliosEngine engine;
        engine.viewport_w = win_w;
        engine.viewport_h = win_h;
        engine.load_bundle(bundle);
        bind_workspace_host_api(engine, workspace);

        // Execute compiled ES6 UI script inside HeliosJS!
        engine.js_rt->eval_script(bundle.js_source);
        engine.render_if_needed(true);

        for (const auto& [cx, cy] : sim_clicks) {
            engine.on_mouse_click(cx, cy);
        }
        for (const auto& ks : sim_keys) {
            for (char c : ks) engine.on_key_input(std::string(1, c), true);
        }

        if (!png_out.empty()) {
            if (fs::path(png_out).has_parent_path()) {
                fs::create_directories(fs::path(png_out).parent_path());
            }
            if (!engine.painter.save_png(png_out)) {
                std::cerr << "[minecc-studio] Failed to save rendered PNG: " << png_out << "\n";
                return 2;
            }
            std::cout << "[minecc-studio] Rendered " << engine.painter.width << "x" << engine.painter.height
                      << " software frame in " << std::fixed << std::setprecision(2) << engine.last_paint_ms
                      << " ms -> " << png_out << "\n";
            return 0;
        }

        if (!serve_addr.empty()) {
            return helios::run_remote_framebuffer_server(engine, serve_addr, bin_dir);
        }

        return helios::run_native_desktop_window(
            engine,
            "minecc-studio — Minecraft 26.3 Native Code Explorer (Helios C++20 HTML5/CSS3/JS Engine)");
    } catch (const std::exception& ex) {
        std::cerr << "[minecc-studio] Fatal error: " << ex.what() << "\n";
        return 1;
    }
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
