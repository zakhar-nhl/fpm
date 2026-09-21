#include "mirror_selector.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <filesystem>
#include <sys/wait.h>

namespace fs = std::filesystem;

static std::string shell_quote(const std::string &s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<Mirror> MirrorSelector::mirrors;
std::string MirrorSelector::selected;
std::string MirrorSelector::selected_file;

const std::string &MirrorSelector::default_mirror()
{
    static const std::string url = "https://geo.mirror.pkgbuild.com/";
    return url;
}

// Returns the first existing file that actually contains at least one mirror
// line (comments/blank-only files are treated as absent, so a stale empty
// template can never shadow a real configuration).
static std::string pick_mirror_file(const std::vector<std::string> &paths)
{
    for (const auto &path : paths)
    {
        std::ifstream in(path);
        if (!in.is_open())
            continue;

        std::string line;
        bool has_entry = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;
            has_entry = true;
            break;
        }
        if (has_entry)
            return path;
    }
    return "";
}

// Active mirrors.list resolution.
//
// Two disjoint config namespaces:
//   * per-user:  a "$HOME/fpm/etc" directory owns the config (tests, custom
//                installs). The system pool NEVER leaks in here, and even an
//                empty mirrors.list counts as "per-user config" (so an empty
//                file means "no mirror configured", not "inherit system").
//   * system:    /etc/fpm/mirrors.list, legacy /etc/mirrors.list and the
//                repo-local etc/mirrors.list, in that order (non-empty wins).
std::string MirrorSelector::find_mirrors_list()
{
    const char *home = std::getenv("HOME");
    if (home)
    {
        std::string etc = std::string(home) + "/fpm/etc";
        std::error_code ec;
        if (fs::is_directory(etc, ec))
        {
            std::string mir = etc + "/mirrors.list";
            if (fs::is_regular_file(mir, ec))
                return mir;
            return "";
        }
    }

    return pick_mirror_file({"/etc/fpm/mirrors.list",
                             "/etc/mirrors.list",
                             "etc/mirrors.list"});
}

// Candidate pool resolution. Rule: the pool is co-located with the active
// mirrors.list, so a per-user config never mixes with the system pool.
std::string MirrorSelector::find_candidates_list()
{
    const char *home = std::getenv("HOME");
    if (home)
    {
        std::string etc = std::string(home) + "/fpm/etc";
        std::error_code ec;
        if (fs::is_directory(etc, ec))
            return pick_mirror_file({etc + "/candidates.list"});
    }

    std::string active = find_mirrors_list();
    if (active == "/etc/fpm/mirrors.list")
        return pick_mirror_file({"/etc/fpm/candidates.list"});
    if (active == "etc/mirrors.list")
        return pick_mirror_file({"etc/candidates.list"});
    return pick_mirror_file({"/etc/fpm/candidates.list"});
}

std::string MirrorSelector::active_default_path()
{
    const char *home = std::getenv("HOME");
    if (home)
    {
        std::string user = std::string(home) + "/fpm/etc/mirrors.list";
        // Per-user setups (tests, homebrew installs) own the config directory.
        if (std::ifstream(user).good() ||
            std::ifstream(std::string(home) + "/fpm/etc/candidates.list").good())
            return user;
    }
    return "/etc/fpm/mirrors.list";
}

std::string MirrorSelector::candidates_default_path()
{
    const char *home = std::getenv("HOME");
    if (home)
    {
        std::string user = std::string(home) + "/fpm/etc/candidates.list";
        if (std::ifstream(std::string(home) + "/fpm/etc/mirrors.list").good() ||
            std::ifstream(user).good())
            return user;
    }
    return "/etc/fpm/candidates.list";
}

std::string MirrorSelector::write_target_mirrors()
{
    std::string user = active_default_path();
    std::string found = find_mirrors_list();
    // Only rewrite genuine per-user / system config. Repo-local etc/ and the
    // legacy /etc/mirrors.list are immutable fallbacks.
    if (found == user || found == "/etc/fpm/mirrors.list")
        return found;
    return user;
}

std::string MirrorSelector::write_target_candidates()
{
    std::string user = candidates_default_path();
    std::string found = find_candidates_list();
    if (found == user || found == "/etc/fpm/candidates.list")
        return found;
    // Co-locate with the active config: a system install writes the system
    // pool, a per-user install writes the per-user pool.
    std::string active = find_mirrors_list();
    if (active == "/etc/fpm/mirrors.list")
        return "/etc/fpm/candidates.list";
    return user;
}

bool MirrorSelector::parse_into(const std::string &path, std::vector<Mirror> &out)
{
    out.clear();
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        Mirror m;
        if (parse_mirror_line(line, m))
            out.push_back(m);
    }

    // Stable: keep file order for equal priority so the mirrors.list order is
    // honored as an intentional preference (no opaque URL tie-breaking).
    std::stable_sort(out.begin(), out.end(),
                     [](const Mirror &a, const Mirror &b)
                     {
                         return a.priority < b.priority;
                     });

    return !out.empty();
}

bool MirrorSelector::load_mirrors(const std::string &list_path)
{
    bool ok = parse_into(list_path, mirrors);
    if (ok)
        load_selected();
    return ok;
}

// Active set: mirrors.list. Falls back to the full candidates pool so that a
// fresh system without an active mirror still has something to try.
bool MirrorSelector::load_active_mirrors()
{
    std::string path = find_mirrors_list();
    if (load_mirrors(path))
        return true;
    return ensure_loaded();
}

bool MirrorSelector::load_candidates()
{
    std::string path = find_candidates_list();
    if (!path.empty() && load_mirrors(path))
        return true;

    // No candidates.list yet: the active list doubles as the pool (-ms).
    path = find_mirrors_list();
    return !path.empty() && load_mirrors(path);
}

bool MirrorSelector::ensure_loaded()
{
    if (!mirrors.empty())
        return true;

    std::string active = find_mirrors_list();
    if (!active.empty() && load_mirrors(active))
        return true;

    std::string cand = find_candidates_list();
    return !cand.empty() && load_mirrors(cand);
}

const std::vector<Mirror> &MirrorSelector::get_mirrors()
{
    return mirrors;
}

// Accepts two formats:
//   1. Plain URL: "https://mirror.example.org/archlinux/"
//   2. Pipe-delimited: "URL | Country | Protocol | Priority"
// Comments (lines starting with '#') and empty lines are ignored.
bool MirrorSelector::parse_mirror_line(const std::string &line, Mirror &m)
{
    if (line.empty() || line[0] == '#')
        return false;

    std::istringstream ss(line);
    std::string token;
    std::vector<std::string> fields;

    while (std::getline(ss, token, '|'))
    {
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            fields.push_back(token.substr(start, end - start + 1));
        else
            fields.push_back("");
    }

    bool is_simple_url = false;
    if (fields.size() == 1 && !fields[0].empty())
    {
        const std::string &u = fields[0];
        if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0 ||
            u.rfind("ftp://", 0) == 0)
        {
            is_simple_url = true;
        }
    }

    m.url = fields[0];

    if (is_simple_url)
    {
        m.country = "";
        m.protocol = m.url.substr(0, m.url.find("://"));
        m.priority = 10;
    }
    else
    {
        if (fields.size() < 4)
            return false;
        m.country = fields[1];
        m.protocol = fields[2];
        try
        {
            m.priority = std::stoi(fields[3]);
        }
        catch (...)
        {
            return false;
        }
    }

    while (!m.url.empty() && m.url.back() == '/')
        m.url.pop_back();

    m.available = true;
    m.rtt_ms = -1;
    m.index = "";
    return !m.url.empty();
}

void MirrorSelector::set_state_path(const std::string &file)
{
    selected_file = file;
    selected.clear();
    load_selected();
}

std::string MirrorSelector::get_selected()
{
    if (selected.empty() && !selected_file.empty())
        load_selected();
    return selected;
}

void MirrorSelector::set_selected(const std::string &url)
{
    selected = url;
    if (selected_file.empty())
        return;

    std::ofstream out(selected_file);
    if (out.is_open())
        out << url << "\n";
}

void MirrorSelector::load_selected()
{
    if (selected_file.empty())
        return;

    std::ifstream in(selected_file);
    if (in.is_open())
    {
        std::getline(in, selected);
        if (!selected.empty() && selected.back() == '\r')
            selected.pop_back();
    }
}

const Mirror *MirrorSelector::select_best()
{
    std::string pref = get_selected();
    if (!pref.empty())
    {
        for (const auto &m : mirrors)
        {
            if (m.url == pref && m.available)
                return &m;
        }
    }

    for (const auto &m : mirrors)
    {
        if (m.available)
            return &m;
    }
    return nullptr;
}

bool MirrorSelector::download(const std::string &url, const std::string &local_path, int timeout_sec)
{
    if (timeout_sec < 1)
        timeout_sec = 15;

    std::string cmd = "curl -sLf --retry 1 --retry-delay 1 --connect-timeout " + std::to_string(timeout_sec) +
                      " --max-time " + std::to_string(timeout_sec * 3) +
                      " --speed-limit 1024 --speed-time 30" +
                      " -o " + shell_quote(local_path) + " " + shell_quote(url) + " 2>/dev/null";

    int status = system(cmd.c_str());
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::error_code ec;
        fs::remove(local_path, ec);
        return false;
    }

    std::error_code ec;
    uintmax_t sz = fs::file_size(local_path, ec);
    if (ec || sz == 0)
    {
        fs::remove(local_path, ec);
        return false;
    }
    return true;
}

// Fast, bounded fetch used for repository index probes: short timeouts, no
// --retry, so probing a chain of broken/hanging mirrors does not take minutes.
bool MirrorSelector::download_fast(const std::string &url, const std::string &local_path)
{
    // Repository index/package downloads can be large (Arch .db, Debian
    // Packages.xz, RPM primary.xml.zst).  Allow generous time and low speed
    // thresholds; connection-refused cases still fail almost instantly due
    // to --connect-timeout.
    std::string cmd = std::string("curl -sLf --connect-timeout 4 --max-time 180") +
                      " --speed-limit 32 --speed-time 60" +
                      " -o " + shell_quote(local_path) + " " + shell_quote(url) + " 2>/dev/null";

    int status = system(cmd.c_str());
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::error_code ec;
        fs::remove(local_path, ec);
        return false;
    }

    std::error_code ec;
    uintmax_t sz = fs::file_size(local_path, ec);
    if (ec || sz == 0)
    {
        fs::remove(local_path, ec);
        return false;
    }
    return true;
}

int MirrorSelector::measure_latency(const std::string &url, int timeout_ms)
{
    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1)
        timeout_sec = 1;

    std::string base = url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    std::string cmd = "curl -s -o /dev/null -L --connect-timeout " + std::to_string(timeout_sec) +
                      " --max-time " + std::to_string(timeout_sec) +
                      " -w '%{time_total}' " + shell_quote(base + "/") + " 2>/dev/null";

    std::array<char, 128> buffer;
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return -1;

    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
        result += buffer.data();
    int rc = pclose(pipe);

    if (rc != 0)
        return -1;

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    try
    {
        double sec = std::stod(result);
        if (sec <= 0.0)
            return -1;
        return static_cast<int>(sec * 1000.0 + 0.5);
    }
    catch (...)
    {
        return -1;
    }
}

std::string MirrorSelector::fetch_text(const std::string &url, int timeout_sec, size_t max_bytes)
{
    std::string cmd = "curl -sLf --connect-timeout " + std::to_string(timeout_sec) +
                      " --max-time " + std::to_string(timeout_sec * 3) +
                      " --max-filesize " + std::to_string(max_bytes) +
                      " " + shell_quote(url) + " 2>/dev/null";

    std::array<char, 4096> buffer;
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return "";

    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
    {
        result += buffer.data();
        if (result.size() > max_bytes)
        {
            result.clear();
            break;
        }
    }
    pclose(pipe);

    return result;
}

const std::vector<std::string> &MirrorSelector::index_candidates()
{
    static const std::vector<std::string> paths = {
        "/packages.db",
        "/packages.json",
        "/index.db",
        "/index.json",
        "/All/packages.db",
        "/x86_64/packages.db",
        "/os/x86_64/packages.db",
        "/repodata/packages.db",
    };
    return paths;
}

int MirrorSelector::http_status(const std::string &url, int timeout_ms)
{
    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1)
        timeout_sec = 1;

    std::string base = url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    std::string cmd = "curl -s -o /dev/null -L --connect-timeout " + std::to_string(timeout_sec) +
                      " --max-time " + std::to_string(timeout_sec) +
                      " -w '%{http_code}' " + shell_quote(base) + " 2>/dev/null";

    std::array<char, 64> buffer;
    std::string code;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return -1;
    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
        code += buffer.data();
    pclose(pipe);

    while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
        code.pop_back();

    // curl prints "000" when the connection never completed (no HTTP response).
    if (code.empty() || code == "000")
        return -1;

    try
    {
        return std::stoi(code);
    }
    catch (...)
    {
        return -1;
    }
}

int MirrorSelector::measure_endpoint(const std::string &url, int timeout_ms)
{
    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1)
        timeout_sec = 1;

    std::string base = url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    std::string cmd = "curl -s -o /dev/null -L -I --connect-timeout " + std::to_string(timeout_sec) +
                      " --max-time " + std::to_string(timeout_sec) +
                      " -w '%{time_total}' " + shell_quote(base) + " 2>/dev/null";

    std::array<char, 128> buffer;
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return -1;

    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
        result += buffer.data();
    int rc = pclose(pipe);

    if (rc != 0)
        return -1;

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    try
    {
        double sec = std::stod(result);
        if (sec <= 0.0)
            return -1;
        return static_cast<int>(sec * 1000.0 + 0.5);
    }
    catch (...)
    {
        return -1;
    }
}

std::vector<Mirror> MirrorSelector::get_ordered_mirrors()
{
    std::vector<Mirror> out;
    std::string pref = get_selected();

    if (!pref.empty())
    {
        for (const auto &m : mirrors)
        {
            if (m.url == pref)
            {
                out.push_back(m);
                break;
            }
        }
    }

    for (const auto &m : mirrors)
    {
        if (m.url != pref)
            out.push_back(m);
    }
    return out;
}

bool MirrorSelector::probe_index(const std::string &base, std::string &found_url,
                                 std::string &found_type, int timeout_ms)
{
    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1)
        timeout_sec = 1;

    std::string root = base;
    while (!root.empty() && root.back() == '/')
        root.pop_back();

    for (const auto &cand : index_candidates())
    {
        std::string url = root + cand;
        std::string cmd = "curl -s -o /dev/null -L --connect-timeout " + std::to_string(timeout_sec) +
                          " --max-time " + std::to_string(timeout_sec) +
                          " -w '%{http_code}' " + shell_quote(url) + " 2>/dev/null";

        std::array<char, 64> buffer;
        std::string code;
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            continue;
        while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
            code += buffer.data();
        pclose(pipe);

        while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
            code.pop_back();

        if (code == "200")
        {
            found_url = url;
            found_type = cand;
            return true;
        }
    }

    return false;
}

// Persists <m> as the single active mirror by rewriting mirrors.list.
// The target is the per-user or system config (never a repo-local or legacy
// fallback file). The reserved default fallback mirror is only re-encoded
// when the mirror IS ALREADY the reserved one, so copying it through -ms
// never degrades the active config to the fallback URL.
bool MirrorSelector::set_active_mirror(const Mirror &m)
{
    std::string target = write_target_mirrors();

    std::string url = trim(m.url);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    while (!url.empty() && url[0] == '/')
        url.erase(url.begin());

    std::string write_url = url;
    std::string fallback_url = default_mirror();
    while (!fallback_url.empty() && fallback_url.back() == '/')
        fallback_url.pop_back();

    if (url == fallback_url)
        write_url = default_mirror();

    std::string country = m.country.empty() ? "Upstream" : m.country;
    std::string proto = m.protocol.empty() ? "https" : m.protocol;
    int priority = m.priority >= 0 ? m.priority : 10;

    std::string content = "# FPM active mirror (managed by fpm -ms / fpm -am).\n" +
                          write_url + " | " + country + " | " + proto + " | " +
                          std::to_string(priority) + "\n";

    std::filesystem::path dir = std::filesystem::path(target).parent_path();
    try
    {
        if (!dir.empty())
            std::filesystem::create_directories(dir);
    }
    catch (...)
    {
        return false;
    }

    std::ofstream out(target, std::ios::trunc);
    if (!out)
        return false;
    out << content;
    out.close();

    // Keep in-memory state consistent with what was persisted.
    if (mirrors.empty())
        load_mirrors(target);
    set_selected(url.empty() ? default_mirror() : url);
    return true;
}

// -am: checks reachability and appends <url> to candidates.list (default) or
// the active mirrors.list (--active). A URL already present is not duplicated.
bool MirrorSelector::add_mirror_candidate(const std::string &url,
                                          const std::string &country,
                                          bool active)
{
    std::string value = trim(url);
    if (value.empty())
        return false;

    while (value.back() == '/')
        value.pop_back();

    const std::string default_url = default_mirror();
    std::string store_url = value + " | " + (country.empty() ? "User-added" : country) +
                            " | https | 10";

    std::string target;
    if (active)
    {
        target = write_target_mirrors();
    }
    else
    {
        target = write_target_candidates();
    }

    // De-duplicate (skip URL match for the reserved default mirror too).
    std::vector<Mirror> existing;
    std::ifstream in(target);
    if (in.is_open())
    {
        std::string line;
        while (std::getline(in, line))
        {
            Mirror m;
            if (parse_mirror_line(line, m))
                existing.push_back(m);
        }
    }

    std::string comp = value;
    std::string default_comp = default_url;
    while (!default_comp.empty() && default_comp.back() == '/')
        default_comp.pop_back();
    if (comp == default_comp)
        comp = default_url;

    for (const auto &m : existing)
    {
        std::string u = m.url;
        if (u == default_comp)
            u = default_url;
        if (u == comp)
            return true; // already present
    }

    std::filesystem::path dir = std::filesystem::path(target).parent_path();
    try
    {
        if (!dir.empty())
            std::filesystem::create_directories(dir);
    }
    catch (...)
    {
        return false;
    }

    std::ofstream out(target, std::ios::app);
    if (!out)
        return false;
    out << store_url << "\n";
    out.close();
    return true;
}

// -dm/-dmm: removes <url> ("my mirror") from BOTH the candidate pool and the
// active mirror list. Non-existent files are skipped; only files that actually
// contained the URL are rewritten (line-preserving, comments untouched).
bool MirrorSelector::remove_mirror(const std::string &url)
{
    std::string value = trim(url);
    if (value.empty())
        return false;
    while (!value.empty() && value.back() == '/')
        value.pop_back();

    std::string target_mirrors = write_target_mirrors();
    std::string target_candidates = write_target_candidates();

    bool removed = false;
    std::string seen;
    for (const std::string &target : {target_candidates, target_mirrors})
    {
        if (target.empty() || target == seen)
            continue;
        seen = target;

        std::ifstream in(target);
        if (!in.is_open())
            continue;

        std::vector<std::string> keep;
        bool modified = false;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            Mirror m;
            if (parse_mirror_line(line, m))
            {
                if (m.url == value)
                {
                    modified = true;
                    continue;
                }
            }
            keep.push_back(line);
        }
        in.close();

        if (!modified)
            continue;

        std::filesystem::path dir = std::filesystem::path(target).parent_path();
        try
        {
            if (!dir.empty())
                std::filesystem::create_directories(dir);
        }
        catch (...)
        {
            return removed;
        }

        std::ofstream out(target, std::ios::trunc);
        if (!out)
            return removed;
        for (const auto &l : keep)
            out << l << "\n";
        out.close();
        removed = true;
    }
    return removed;
}

// Lightweight reachability probe: any HTTP reply (even 404) counts as
// reachable, since index probing is a separate, slower signal.
bool MirrorSelector::is_reachable(const std::string &url, int timeout_ms)
{
    return http_status(url, timeout_ms) != -1;
}

// Lightweight format detection: checks well-known index marker paths for a
// 200 response. Used by -ms so the full candidate pool can be triaged with
// cheap HEAD-style probes instead of downloading every index.
bool MirrorSelector::probe_repo_marker(const std::string &base, std::string &found_type,
                                       int timeout_ms)
{
    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1)
        timeout_sec = 1;

    std::string root = base;
    while (!root.empty() && root.back() == '/')
        root.pop_back();

    static const std::vector<std::string> markers = {
        "/packages.db",                // FPM native
        "/packages.json",              // FPM native (JSON)
        "/index.db",                   // FPM native (index.db)
        "/core/os/x86_64/core.db",     // Arch Linux
        "/dists/stable/Release",       // Debian (stable)
        "/dists/bookworm/Release",     // Debian 12
        "/dists/noble/Release",        // Ubuntu 24.04
        "/dists/jammy/Release",        // Ubuntu 22.04
        "/APKINDEX.tar.gz",            // Alpine Linux
        "/repodata/repomd.xml",        // RPM / DNF (Fedora, RHEL)
    };

    for (const auto &marker : markers)
    {
        std::string url = root + marker;
        std::string cmd = "curl -s -o /dev/null -L --connect-timeout " + std::to_string(timeout_sec) +
                          " --max-time " + std::to_string(timeout_sec) +
                          " -w '%{http_code}' " + shell_quote(url) + " 2>/dev/null";

        std::array<char, 64> buffer;
        std::string code;
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            continue;
        while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
            code += buffer.data();
        pclose(pipe);

        while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
            code.pop_back();

        if (code == "200")
        {
            found_type = marker;
            return true;
        }
    }

    return false;
}