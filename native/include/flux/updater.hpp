#pragma once
// FluxUpdater: Non-Blocking Startup GitHub Release Auto-Updater
//   - Checks `https://api.github.com/repos/92mxs21/minecc/releases/latest` asynchronously on every launch
//     (WinHTTP native TLS on Windows, non-blocking `curl` subprocess on Linux — ZERO main-thread wait!).
//   - Displays crisp spring-animated banner with playful developer prompts + 1-click live binary self-update!

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <unistd.h>
#endif

namespace flux::updater {

inline constexpr const char* CURRENT_VERSION = "2.1.0";
inline constexpr const char* GITHUB_REPO = "92mxs21/minecc";

// Casual playful developer update messages requested by user :)
inline std::string pick_funny_update_headline(uint32_t seed = 0) {
    static constexpr std::array<const char*, 5> LINES = {{
        "yoooo brooo update (pls update fixes things)",
        "yooo bro update rn (pls update fixes random shit)",
        "brooo new update dropped (pls update unbreaks weird bugs)",
        "ayoo update available (pls update makes decompiler way cleaner)",
        "yo twin update real quick (pls update literally fixes sm stuff)",
    }};
    return LINES[seed % LINES.size()];
}

inline int parse_semver_key(std::string s) noexcept {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
    int maj = 0, min = 0, pat = 0;
    std::sscanf(s.c_str(), "%d.%d.%d", &maj, &min, &pat);
    return maj * 1000000 + min * 1000 + pat;
}

inline std::string extract_json_str(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = q1 + 1;
    while (q2 < json.size() && json[q2] != '"') {
        if (json[q2] == '\\' && q2 + 1 < json.size()) ++q2;
        ++q2;
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline bool https_get_bytes(const std::string& url, std::vector<uint8_t>& out, std::atomic<float>* progress = nullptr) {
    out.clear();
#if defined(_WIN32)
    // Parse `https://host/path` cleanly for native OS TLS via WinHTTP
    size_t scheme = url.find("://");
    size_t host_start = (scheme == std::string::npos) ? 0 : scheme + 3;
    size_t slash = url.find('/', host_start);
    std::string host = (slash == std::string::npos) ? url.substr(host_start) : url.substr(host_start, slash - host_start);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSession = WinHttpOpen(L"minecc-updater/2.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 4000, 4000, 8000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = WinHttpSendRequest(hReq, L"Accept: application/vnd.github+json\r\n", DWORD(-1),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hReq, nullptr);
    if (ok) {
        DWORD total_len = 0, sz_len = sizeof(total_len);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &total_len, &sz_len, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            size_t prev = out.size();
            out.resize(prev + avail);
            DWORD got = 0;
            if (!WinHttpReadData(hReq, out.data() + prev, avail, &got)) break;
            out.resize(prev + got);
            if (progress && total_len > 0) {
                progress->store(std::min(0.98f, float(out.size()) / float(total_len)));
            }
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok && !out.empty();
#else
    std::string cmd = "curl -fsSL --max-time 6 -H 'Accept: application/vnd.github+json' -H 'User-Agent: minecc-updater/2.1' '" + url + "' 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return false;
    uint8_t buf[8192];
    while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
        out.insert(out.end(), buf, buf + n);
        if (progress) progress->store(std::min(0.95f, float(out.size()) / 1048576.0f));
    }
    int rc = ::pclose(pipe);
    return rc == 0 && !out.empty();
#endif
}

class AutoUpdater {
public:
    std::atomic<bool> checking{false};
    std::atomic<bool> update_available{false};
    std::atomic<bool> dismissed{false};
    std::atomic<bool> downloading{false};
    std::atomic<bool> restart_ready{false};
    std::atomic<float> download_progress{0.0f};

    mutable std::mutex mtx;
    std::string latest_tag = "v2.1.0";
    std::string headline = pick_funny_update_headline(0);
    std::string detail_line = "Checking GitHub Releases...";
    std::string download_url;
    std::string self_exe_path;

    std::thread check_thread;
    std::thread dl_thread;

    ~AutoUpdater() {
        if (check_thread.joinable()) check_thread.join();
        if (dl_thread.joinable()) dl_thread.join();
    }

    void start_async_check(std::string exe_path, bool simulate_outdated = false) {
        self_exe_path = std::move(exe_path);
        if (checking.exchange(true)) return;
        if (check_thread.joinable()) check_thread.join();

        check_thread = std::thread([this, simulate_outdated]() {
            uint32_t seed = static_cast<uint32_t>(std::random_device{}());
            if (simulate_outdated) {
                std::lock_guard<std::mutex> lk(mtx);
                latest_tag = "v2.2.0";
                headline = pick_funny_update_headline(seed);
                detail_line = "v" + std::string(CURRENT_VERSION) + " -> " + latest_tag + "  ·  Faster C++20 CFG & UI fixes";
#if defined(_WIN32)
                download_url = "https://github.com/92mxs21/minecc/releases/latest/download/minecc-windows-x86_64.exe";
#else
                download_url = "https://github.com/92mxs21/minecc/releases/latest/download/minecc-linux-x86_64";
#endif
                dismissed.store(false);
                update_available.store(true);
                checking.store(false);
                return;
            }

            std::vector<uint8_t> resp;
            std::string api = std::string("https://api.github.com/repos/") + GITHUB_REPO + "/releases/latest";
            if (https_get_bytes(api, resp)) {
                std::string body(resp.begin(), resp.end());
                std::string tag = extract_json_str(body, "tag_name");
                if (!tag.empty() && parse_semver_key(tag) > parse_semver_key(CURRENT_VERSION)) {
                    std::lock_guard<std::mutex> lk(mtx);
                    latest_tag = tag;
                    headline = pick_funny_update_headline(seed);
                    detail_line = "Current v" + std::string(CURRENT_VERSION) + " -> " + tag + " available on GitHub";
#if defined(_WIN32)
                    download_url = "https://github.com/92mxs21/minecc/releases/download/" + tag + "/minecc-windows-x86_64.exe";
#else
                    download_url = "https://github.com/92mxs21/minecc/releases/download/" + tag + "/minecc-linux-x86_64";
#endif
                    dismissed.store(false);
                    update_available.store(true);
                } else if (!tag.empty()) {
                    std::lock_guard<std::mutex> lk(mtx);
                    latest_tag = tag;
                    detail_line = "Up to date (" + tag + ")";
                }
            }
            checking.store(false);
        });
    }

    void trigger_install_and_restart() {
        if (downloading.exchange(true)) return;
        download_progress.store(0.05f);
        if (dl_thread.joinable()) dl_thread.join();

        dl_thread = std::thread([this]() {
            std::string url, target_exe;
            {
                std::lock_guard<std::mutex> lk(mtx);
                url = download_url;
                target_exe = self_exe_path;
            }
            std::vector<uint8_t> bin;
            if (!url.empty() && https_get_bytes(url, bin, &download_progress) && bin.size() > 65536) {
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path self_p(target_exe);
                if (!target_exe.empty() && fs::exists(self_p, ec)) {
                    fs::path bak = target_exe + ".old";
                    fs::path tmp = target_exe + ".new";
                    {
                        std::ofstream out(tmp, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(bin.data()), std::streamsize(bin.size()));
                    }
#if defined(_WIN32)
                    MoveFileExA(target_exe.c_str(), bak.string().c_str(), MOVEFILE_REPLACE_EXISTING);
                    MoveFileExA(tmp.string().c_str(), target_exe.c_str(), MOVEFILE_REPLACE_EXISTING);
                    STARTUPINFOA si{}; si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessA(target_exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        download_progress.store(1.0f);
                        restart_ready.store(true);
                        ExitProcess(0);
                    }
#else
                    fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, ec);
                    fs::rename(tmp, self_p, ec);
                    download_progress.store(1.0f);
                    restart_ready.store(true);
#endif
                }
            } else {
                std::lock_guard<std::mutex> lk(mtx);
                detail_line = "Open https://github.com/92mxs21/minecc/releases/latest";
            }
            downloading.store(false);
        });
    }
};

} // namespace flux::updater
