#include "cleaner.hpp"
#include "database.hpp"
#include "ui.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static uint64_t dir_bytes(const fs::path &dir)
{
    uint64_t total = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return 0;

    try
    {
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            std::error_code lec;
            if (it->is_regular_file(lec) && !lec)
                total += it->file_size(lec);
        }
    }
    catch (...)
    {
    }
    return total;
}

// Removes everything inside <dir> (files and directories) without touching
// <dir> itself. Handles missing dirs, permission errors and broken entries.
uint64_t Cleaner::remove_directory_contents(const std::string &dir)
{
    fs::path d(dir);
    uint64_t total = 0;
    std::error_code ec;
    if (!fs::exists(d, ec) || ec)
        return 0;

    try
    {
        for (auto it = fs::directory_iterator(d, ec); it != fs::directory_iterator(); ++it)
        {
            std::error_code lec;
            const fs::path p = it->path();

            // Do not follow symlinks: a symlink to a directory is removed as a
            // link, never traversed.
            if (fs::is_directory(p, lec) && !fs::is_symlink(p, lec) && !lec)
                total += dir_bytes(p);
            else
            {
                uintmax_t sz = fs::file_size(p, lec);
                if (!lec)
                    total += sz;
            }

            std::error_code rec;
            fs::remove_all(p, rec); // removes dirs recursively, leaves top dir
        }
    }
    catch (...)
    {
    }
    return total;
}

uint64_t Cleaner::remove_tmp_dirs(const std::string &pattern)
{
    uint64_t total = 0;
    fs::path root;
    std::string prefix;

    size_t slash = pattern.find_last_of('/');
    if (slash == std::string::npos)
        return 0;

    root = fs::path(pattern.substr(0, slash));
    prefix = pattern.substr(slash + 1);

    if (prefix.empty() || prefix.back() != '*')
        return 0;
    prefix.pop_back();

    std::error_code ec;
    if (!fs::exists(root, ec))
        return 0;

    try
    {
        for (const auto &entry : fs::directory_iterator(root, ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0)
            {
                total += dir_bytes(entry.path());
                fs::remove_all(entry.path(), ec);
            }
        }
    }
    catch (...)
    {
    }

    return total;
}

CleanStats Cleaner::clean_cache()
{
    CleanStats stats = {};
    std::string pkg_cache = DatabaseManager::get_package_cache_path();

    stats.bytes_freed = remove_directory_contents(pkg_cache);

    return stats;
}

CleanStats Cleaner::clean_package_cache()
{
    CleanStats stats = {};
    std::string pkg_cache = DatabaseManager::get_package_cache_path();
    stats.bytes_freed = remove_directory_contents(pkg_cache);
    return stats;
}

uint64_t Cleaner::get_cache_size()
{
    return dir_bytes(DatabaseManager::get_package_cache_path());
}

std::vector<std::string> Cleaner::clean_deep(uint64_t &bytes_freed_out)
{
    std::vector<std::string> report;
    bytes_freed_out = 0;
    DatabaseManager::init();

    const std::string pkg_cache = DatabaseManager::get_package_cache_path();
    uint64_t archives = remove_directory_contents(pkg_cache);

    const std::string repo_cache = DatabaseManager::get_repo_cache_path();
    uint64_t indexes = remove_directory_contents(repo_cache);

    uint64_t cache_freed = archives + indexes;
    report.push_back("Cache cleaned:      " + UI::format_size(cache_freed));

    uint64_t tmp_build = remove_tmp_dirs("/tmp/fpm_*") +
                         remove_tmp_dirs("/tmp/fpm-stage-*") +
                         remove_tmp_dirs("/var/tmp/fpm_*");
    report.push_back("Temporary cleaned:  " + UI::format_size(tmp_build));

    bytes_freed_out = cache_freed + tmp_build;
    report.push_back("Total freed:        " + UI::format_size(bytes_freed_out));
    return report;
}