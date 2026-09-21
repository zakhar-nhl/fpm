#include "database.hpp"
#include "fuzzy_search.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include <cctype>

namespace fs = std::filesystem;

// Compares version strings like "1.0.0" / "2.0.0-r1" numerically per dot/dash segment.
static int compare_versions(const std::string &a, const std::string &b)
{
    std::vector<std::string> sa, sb;

    auto split = [](const std::string &s, std::vector<std::string> &out)
    {
        std::string cur;
        for (char c : s)
        {
            if (c == '.' || c == '-' || c == '_' || c == '+')
            {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            }
            else
            {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            out.push_back(cur);
    };

    split(a, sa);
    split(b, sb);

    size_t n = std::max(sa.size(), sb.size());
    for (size_t i = 0; i < n; ++i)
    {
        long va = 0, vb = 0;
        bool na = i < sa.size() && std::all_of(sa[i].begin(), sa[i].end(), ::isdigit);
        bool nb = i < sb.size() && std::all_of(sb[i].begin(), sb[i].end(), ::isdigit);

        if (na)
            va = std::stol(i < sa.size() ? sa[i] : "0");
        if (nb)
            vb = std::stol(i < sb.size() ? sb[i] : "0");

        if (na && nb)
        {
            if (va != vb)
                return va < vb ? -1 : 1;
        }
        else
        {
            std::string pa = i < sa.size() ? sa[i] : "";
            std::string pb = i < sb.size() ? sb[i] : "";
            if (pa != pb)
                return pa < pb ? -1 : 1;
        }
    }
    return 0;
}

std::string DatabaseManager::db_dir = "./fpm_db";
std::string DatabaseManager::cache_dir = "./fpm_cache";

void DatabaseManager::set_db_path(const std::string &path)
{
    db_dir = path;
}

void DatabaseManager::set_cache_path(const std::string &path)
{
    cache_dir = path;
}

std::string DatabaseManager::get_db_path()
{
    return db_dir;
}

std::string DatabaseManager::get_local_path()
{
    return db_dir + "/local";
}

std::string DatabaseManager::get_repo_cache_path()
{
    return cache_dir + "/repo";
}

std::string DatabaseManager::get_package_cache_path()
{
    return cache_dir + "/packages";
}

std::string DatabaseManager::get_remote_index_path()
{
    return db_dir + "/remote_packages.db";
}

std::string DatabaseManager::get_selected_mirror_path()
{
    return db_dir + "/selected_mirror";
}

std::string DatabaseManager::install_meta_path(const std::string &name)
{
    return get_local_path() + "/" + name + "/meta";
}

std::string DatabaseManager::install_manifest_path(const std::string &name)
{
    return get_local_path() + "/" + name + "/manifest";
}

bool DatabaseManager::init()
{
    try
    {
        fs::create_directories(get_local_path());
        fs::create_directories(get_repo_cache_path());
        fs::create_directories(get_package_cache_path());
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Database Error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::write_meta(const std::string &path, const InstalledPackageInfo &pkg)
{
    fs::path p(path);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());

    std::ofstream out(path);
    if (!out.is_open())
        return false;

    out << "NAME=" << pkg.name << "\n";
    out << "VERSION=" << pkg.version << "\n";
    out << "ARCH=" << pkg.arch << "\n";
    out << "REPO=" << pkg.repo << "\n";
    out << "SIZE=" << pkg.size << "\n";
    out << "DESC=" << pkg.description << "\n";
    if (!pkg.sha256.empty())
        out << "SHA256=" << pkg.sha256 << "\n";
    out << "EXPLICIT=" << (pkg.explicit_install ? "1" : "0") << "\n";
    return true;
}

bool DatabaseManager::read_meta(const std::string &path, InstalledPackageInfo &pkg)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    pkg = InstalledPackageInfo();
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "NAME")
            pkg.name = val;
        else if (key == "VERSION")
            pkg.version = val;
        else if (key == "ARCH")
            pkg.arch = val;
        else if (key == "REPO")
            pkg.repo = val;
        else if (key == "SIZE")
            pkg.size = val;
        else if (key == "DESC")
            pkg.description = val;
        else if (key == "SHA256")
            pkg.sha256 = val;
        else if (key == "EXPLICIT")
            pkg.explicit_install = (val == "1");
    }
    return !pkg.name.empty();
}

bool DatabaseManager::write_manifest(const std::string &path, const std::vector<std::string> &files,
                                     const std::vector<std::string> &file_hashes)
{
    std::ofstream out(path);
    if (!out.is_open())
        return false;

    for (size_t i = 0; i < files.size(); ++i)
    {
        std::string hash = (i < file_hashes.size()) ? file_hashes[i] : "";
        out << (hash.empty() ? "-" : hash) << " " << files[i] << "\n";
    }
    return true;
}

bool DatabaseManager::read_manifest(const std::string &path, std::vector<std::string> &files,
                                    std::vector<std::string> &file_hashes)
{
    files.clear();
    file_hashes.clear();
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        size_t sp = line.find(' ');
        std::string hash;
        std::string file;
        if (sp == std::string::npos)
        {
            file = line;
        }
        else
        {
            hash = line.substr(0, sp);
            file = line.substr(sp + 1);
        }
        if (hash == "-")
            hash.clear();
        files.push_back(file);
        file_hashes.push_back(hash);
    }
    return true;
}

bool DatabaseManager::is_installed(const std::string &name)
{
    return fs::exists(install_meta_path(name));
}

bool DatabaseManager::add_package(const InstalledPackageInfo &pkg)
{
    init();

    std::string pkg_dir = get_local_path() + "/" + pkg.name;
    fs::create_directories(pkg_dir);

    if (!write_meta(install_meta_path(pkg.name), pkg))
        return false;

    if (!pkg.files.empty())
        write_manifest(install_manifest_path(pkg.name), pkg.files, pkg.file_hashes);

    return true;
}

bool DatabaseManager::remove_package(const std::string &name)
{
    std::string pkg_dir = get_local_path() + "/" + name;
    if (fs::exists(pkg_dir))
    {
        std::error_code ec;
        fs::remove_all(pkg_dir, ec);
        return !ec;
    }
    return false;
}

bool DatabaseManager::get_package(const std::string &name, InstalledPackageInfo &pkg)
{
    std::string meta = install_meta_path(name);
    if (!fs::exists(meta))
        return false;

    if (!read_meta(meta, pkg))
        return false;

    read_manifest(install_manifest_path(name), pkg.files, pkg.file_hashes);
    return true;
}

std::vector<InstalledPackageInfo> DatabaseManager::get_all_packages()
{
    init();
    std::vector<InstalledPackageInfo> list;

    fs::path local_dir = get_local_path();
    if (!fs::exists(local_dir))
        return list;

    for (const auto &entry : fs::directory_iterator(local_dir))
    {
        if (entry.is_directory())
        {
            std::string pkg_name = entry.path().filename().string();
            InstalledPackageInfo pkg;
            if (get_package(pkg_name, pkg))
                list.push_back(pkg);
        }
    }

    std::sort(list.begin(), list.end(),
              [](const InstalledPackageInfo &a, const InstalledPackageInfo &b)
              { return a.name < b.name; });

    return list;
}

std::vector<std::string> DatabaseManager::get_installed_files(const std::string &name)
{
    std::vector<std::string> files;
    std::vector<std::string> hashes;
    read_manifest(install_manifest_path(name), files, hashes);
    return files;
}

bool DatabaseManager::save_repo_index(const std::string &repo_name, const std::string &content)
{
    init();
    std::string path = get_repo_cache_path() + "/" + repo_name + ".db";
    std::ofstream out(path);
    if (!out.is_open())
        return false;
    out << content;
    return true;
}

bool DatabaseManager::load_repo_index(const std::string &repo_name, std::string &content)
{
    std::string path = get_repo_cache_path() + "/" + repo_name + ".db";
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

static RepoPackage parse_repo_line(const std::string &line)
{
    RepoPackage pkg;
    std::istringstream ss(line);
    std::string token;

    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.name = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.version = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.arch = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.repo = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.size = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.description = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.sha256 = token.substr(s, e - s + 1);
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
        {
            std::string deps = token.substr(s, e - s + 1);
            std::istringstream ds(deps);
            std::string dep;
            while (std::getline(ds, dep, ','))
            {
                size_t ds_ = dep.find_first_not_of(" \t");
                size_t de_ = dep.find_last_not_of(" \t");
                if (ds_ != std::string::npos)
                    pkg.depends.push_back(dep.substr(ds_, de_ - ds_ + 1));
            }
        }
    }
    if (std::getline(ss, token, '|'))
    {
        size_t s = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (s != std::string::npos)
            pkg.url = token.substr(s, e - s + 1);
    }

    return pkg;
}

static std::string serialize_repo_package(const RepoPackage &pkg)
{
    std::string line = pkg.name + " | " + pkg.version + " | " + pkg.arch + " | " +
                       pkg.repo + " | " + pkg.size + " | " + pkg.description + " | " +
                       pkg.sha256 + " | ";

    for (size_t i = 0; i < pkg.depends.size(); ++i)
    {
        if (i > 0)
            line += ",";
        line += pkg.depends[i];
    }

    line += " | " + pkg.url;
    return line;
}

// --- Unified remote index (remote_packages.db) ---

bool DatabaseManager::save_remote_index(const std::vector<RepoPackage> &packages)
{
    init();
    std::string path = get_remote_index_path();
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out.is_open())
        return false;

    out << "# FPM remote package index (canonical text format)\n";
    out << "# name|version|arch|repo|size|description|sha256|depends|url\n";

    for (const auto &pkg : packages)
        out << serialize_repo_package(pkg) << "\n";
    out.flush();
    if (!out.good())
    {
        out.close();
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    out.close();

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool DatabaseManager::save_repo_splits(const std::vector<RepoPackage> &packages)
{
    init();
    std::map<std::string, std::vector<RepoPackage>> by_repo;
    for (const auto &pkg : packages)
        by_repo[pkg.repo.empty() ? "core" : pkg.repo].push_back(pkg);

    bool ok = true;
    for (const auto &kv : by_repo)
    {
        std::string path = get_repo_cache_path() + "/" + kv.first + ".db";
        std::string tmp = path + ".tmp";
        std::ofstream out(tmp);
        if (!out.is_open())
        {
            ok = false;
            continue;
        }
        for (const auto &pkg : kv.second)
            out << serialize_repo_package(pkg) << "\n";
        out.close();
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec)
        {
            fs::remove(tmp, ec);
            ok = false;
        }
    }
    return ok;
}

static std::vector<RepoPackage> scan_repo_dir_text_files()
{
    std::vector<RepoPackage> results;

    fs::path repo_dir = DatabaseManager::get_repo_cache_path();
    if (!fs::exists(repo_dir))
        return results;

    for (const auto &entry : fs::directory_iterator(repo_dir))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".db")
            continue;

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.is_open())
            continue;

        char probe[64] = {};
        in.read(probe, sizeof(probe));
        bool is_text = in.gcount() > 0;
        for (std::streamsize i = 0; i < in.gcount(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(probe[i]);
            if (c == '\0' || c > 127)
            {
                is_text = false;
                break;
            }
        }
        if (!is_text)
            continue;
        in.seekg(0);

        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            RepoPackage pkg = parse_repo_line(line);
            if (!pkg.name.empty())
                results.push_back(pkg);
        }
    }

    return results;
}

std::vector<RepoPackage> DatabaseManager::load_remote_index()
{
    // Lazy per-process cache; re-reads only when the canonical index changed.
    static bool have_cache = false;
    static fs::file_time_type cache_time{};
    static std::vector<RepoPackage> cache;

    std::string path = get_remote_index_path();
    std::error_code st_ec;
    auto stamp = fs::last_write_time(path, st_ec);

    if (!st_ec && have_cache && stamp == cache_time && !cache.empty())
        return cache;

    std::vector<RepoPackage> results;
    std::ifstream in(path);
    if (!in.is_open())
        results = scan_repo_dir_text_files();
    else
    {
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            RepoPackage pkg = parse_repo_line(line);
            if (!pkg.name.empty())
                results.push_back(pkg);
        }

        if (results.empty())
            results = scan_repo_dir_text_files();
    }

    if (!st_ec)
    {
        have_cache = true;
        cache_time = stamp;
        cache = results;
    }
    return results;
}

std::vector<RepoPackage> DatabaseManager::search_repo(const std::string &query, bool fuzzy)
{
    std::vector<RepoPackage> all = load_remote_index();

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    if (lower_query.empty())
        return {};

    // Ranked hit: tier drives the ordering buckets, sim breaks ties inside the
    // fuzzy buckets (higher similarity first).
    struct Ranked
    {
        RepoPackage pkg;
        int tier;
        double sim;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(all.size() / 8);

    // Name matches are always allowed. Description and typo-tolerant matches
    // are part of the deep search (-fs) only, so that -s stays a strict name
    // search. -fs pyhton falls through to tier 4/5 for "python" and the whole
    // python-* family instead of stopping at the exact typo target.
    for (auto &pkg : all)
    {
        std::string lower_name = pkg.name;
        std::string lower_desc = pkg.description;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(), ::tolower);

        int tier = -1;
        double sim = 0.0;

        if (lower_name == lower_query)
        {
            tier = 0;
        }
        else if (lower_name.rfind(lower_query, 0) == 0)
        {
            tier = 1;
        }
        else if (lower_name.find(lower_query) != std::string::npos)
        {
            tier = 2;
        }
        else if (fuzzy && lower_query.size() >= 2 &&
                 lower_desc.find(lower_query) != std::string::npos)
        {
            tier = 3;
        }
        else if (fuzzy && FuzzySearch::plausibly_matches(lower_query, lower_name))
        {
            tier = 4;
            sim = FuzzySearch::similarity(lower_query, lower_name);
        }
        else if (fuzzy && FuzzySearch::fuzzy_prefix_matches(lower_query, lower_name))
        {
            tier = 5;
            std::string prefix = lower_name.substr(0, lower_query.size());
            sim = FuzzySearch::similarity(lower_query, prefix);
        }

        if (tier >= 0)
            ranked.push_back({std::move(pkg), tier, sim});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked &a, const Ranked &b)
              {
                  if (a.tier != b.tier)
                      return a.tier < b.tier;
                  if (a.tier >= 4 && a.sim != b.sim)
                      return a.sim > b.sim;
                  if (a.pkg.name.size() != b.pkg.name.size())
                      return a.pkg.name.size() < b.pkg.name.size();
                  return a.pkg.name < b.pkg.name;
              });

    std::vector<RepoPackage> results;
    results.reserve(ranked.size());
    for (auto &r : ranked)
        results.push_back(std::move(r.pkg));
    return results;
}

bool DatabaseManager::get_repo_package(const std::string &name, RepoPackage &pkg)
{
    bool found = false;
    std::string wanted = name;

    auto try_find = [&](const std::string &n) -> bool
    {
        bool f = false;
        for (const auto &p : load_remote_index())
        {
            if (p.name == n)
            {
                if (!f || compare_versions(p.version, pkg.version) > 0)
                    pkg = p;
                f = true;
            }
        }
        return f;
    };

    found = try_find(wanted);

    // Version-constrained dependency names ("glibc>=2.35") resolve to the
    // plain package name as a fallback.
    if (!found)
    {
        size_t p = wanted.find_first_of(">=<");
        if (p != std::string::npos)
        {
            wanted = wanted.substr(0, p);
            while (!wanted.empty() && (wanted.back() == ' ' || wanted.back() == '\t'))
                wanted.pop_back();
            found = try_find(wanted);
        }
    }
    return found;
}

std::vector<RepoPackage> DatabaseManager::get_repo_packages(const std::string &repo)
{
    std::vector<RepoPackage> results;

    for (const auto &pkg : load_remote_index())
    {
        if (repo.empty() || pkg.repo == repo)
            results.push_back(pkg);
    }

    return results;
}
