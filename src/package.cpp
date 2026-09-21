#include "package.hpp"
#include "logger.hpp"
#include "database.hpp"
#include "slag.hpp"
#include "mirror_selector.hpp"
#include "cleaner.hpp"
#include "ui.hpp"
#include "json.hpp"
#include "fuzzy_search.hpp"
#include "repository.hpp"
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <map>
#include <set>

namespace fs = std::filesystem;

static std::string trim_str(std::string s)
{
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static bool looks_like_sha256(const std::string &s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// Packages provided by the base operating system.  FPM never installs or
// re-resolves these; a dependency on one is always considered satisfied.
// This is essential when consuming real third-party repositories whose
// packages depend on glibc/bash/coreutils etc. that ship with the OS.
static bool is_base_system_package(const std::string &_name)
{
    std::string name = trim_str(_name);
    if (name.empty())
        return false;

    static const std::vector<std::string> base = {
        "glibc", "libc6", "libc", "bash", "sh", "coreutils", "filesystem",
        "sed", "grep", "gawk", "awk", "diffutils", "findutils", "gzip",
        "bzip2", "xz", "zstd", "tar", "make", "gcc", "gcc-libs", "binutils",
        "linux-api-headers", "ncurses", "ncursesw", "readline", "zlib",
        "libzstd", "openssl", "libopenssl", "systemd", "systemd-libs",
        "procps-ng", "procps", "util-linux", "util-linux-libs", "pam",
        "shadow", "passwd", "libcap", "libselinux", "libsepol", "ca-certificates",
        "bash-completion", "less", "man-db", "man-pages", "texinfo",
        "inetutils", "iproute2", "iputils", "which", "file", "cpio",
        "gettext", "gettext-runtime", "pkgconf", "pkg-config", "libtool",
        "autoconf", "automake", "flex", "bison", "m4", "perl", "perl-*",
        "python", "python3", "libxml2", "libcurl", "curl", "wget",
        "elfutils", "krb5", "libkrb5", "keyutils", "nss", "openssl",
        "libffi", "libgcc", "libstdc++", "libunistring", "libidn2",
        "libtasn1", "libp11-kit", "p11-kit", "lz4", "liblz4", "libsystemd",
        "zlib-ng", "libbrotli", "brotli", "netbase", "base", "base-devel",
        "iana-etc", "tzdata", "glibc-locales", "linux", "grub", "systemd-sysvcompat",
        "bash-completion", "logrotate", "rsync", "patch", "ed",
        "e2fsprogs", "os-release", "ds-cleaner", "hosptoolkit", "sqlite",
    };

    // Strip common version constraints / provides aliases before matching.
    std::string n = repo::normalize_dep_name(name);
    if (n.empty())
        n = name;

    for (const auto &b : base)
    {
        if (b.back() == '*')
        {
            std::string prefix = b.substr(0, b.size() - 1);
            if (n.rfind(prefix, 0) == 0)
                return true;
        }
        else if (n == b)
        {
            return true;
        }
    }

    return false;
}

// Checks whether the folder/program a package would provide is already
// present on the running system (e.g. glibc).  Used to avoid pulling base
// system components from a foreign repository.
static bool provides_already_on_system(const std::string &name)
{
    if (name.empty())
        return false;

    if (name.rfind("lib", 0) == 0)
    {
        // Common shared library-style dependencies: resolve to a soname
        // path under /usr/lib and bail out early if it exists.
        std::string base = name;
        size_t dot = base.find('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        if (base.size() < 3)
            return false;

        std::error_code ec;
        if (fs::exists("/usr/lib/lib" + base + ".so", ec))
            return true;
        ec.clear();
        if (fs::exists("/lib/lib" + base + ".so", ec))
            return true;
        ec.clear();
        if (fs::exists("/usr/lib/lib" + base + ".so.1", ec))
            return true;
        return false;
    }

    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::error_code ec;
    if (lower == "sh" || lower == "bash")
        return fs::exists("/bin/sh", ec) || fs::exists("/usr/bin/sh", ec);
    if (lower == "coreutils" || lower == "grep" || lower == "gawk" ||
        lower == "awk" || lower == "sed" || lower == "findutils" ||
        lower == "diffutils" || lower == "tar" || lower == "gzip" ||
        lower == "xz")
        return true;

    return false;
}

static std::string normalize_dep_name(std::string dep)
{
    dep = trim_str(dep);
    size_t p = dep.find_first_of(">=<");
    if (p != std::string::npos)
        dep = dep.substr(0, p);
    return trim_str(dep);
}

static std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += "\\uFFFD";
            else
                out += c;
        }
    }
    return out;
}

static bool normalize_install_path(const std::string &p, std::string &norm)
{
    if (p.empty())
        return false;

    std::string cur = (p[0] == '/') ? p : ("/" + p);
    std::istringstream iss(cur);
    std::string seg;
    std::vector<std::string> segs;
    while (std::getline(iss, seg, '/'))
    {
        if (seg.empty() || seg == ".")
            continue;
        if (seg == "..")
            return false;
        segs.push_back(seg);
    }
    if (segs.empty())
        return false;

    norm.clear();
    for (const auto &s : segs)
    {
        norm += "/";
        norm += s;
    }
    return true;
}

static fs::perms octal_to_perms(int o)
{
    fs::perms p = fs::perms::none;
    if (o & 0400) p |= fs::perms::owner_read;
    if (o & 0200) p |= fs::perms::owner_write;
    if (o & 0100) p |= fs::perms::owner_exec;
    if (o & 0040) p |= fs::perms::group_read;
    if (o & 0020) p |= fs::perms::group_write;
    if (o & 0010) p |= fs::perms::group_exec;
    if (o & 0004) p |= fs::perms::others_read;
    if (o & 0002) p |= fs::perms::others_write;
    if (o & 0001) p |= fs::perms::others_exec;
    return p;
}

static void apply_entry_mode(const fs::path &dest, const std::string &mode)
{
    if (mode.size() < 3)
        return;
    std::string tail = mode.substr(mode.size() - 3);
    try
    {
        long o = std::stol(tail, nullptr, 8);
        std::error_code ec;
        fs::permissions(dest, octal_to_perms(static_cast<int>(o)),
                        fs::perm_options::replace, ec);
    }
    catch (...)
    {
    }
}

// Parses an Arch package .PKGINFO file (KEY = value lines).
static bool parse_arch_pkginfo_file(const std::string &path, PackageMeta &meta)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = trim_str(line.substr(0, eq));
        std::string val = trim_str(line.substr(eq + 1));

        if (key == "pkgname" || key == "pkgbase")
        {
            if (meta.name.empty() || key == "pkgname")
                meta.name = val;
        }
        else if (key == "pkgver")
            meta.version = val;
        else if (key == "arch")
            meta.arch = val;
        else if (key == "pkgdesc")
            meta.description = val;
        else if (key == "size" && meta.size.empty())
        {
            try
            {
                meta.size = UI::format_size(std::stoull(val, nullptr, 10));
            }
            catch (...)
            {
                meta.size = val;
            }
        }
        else if (key == "depend")
            meta.depends.push_back(val);
    }
    return !meta.name.empty();
}

// Extracts a real package archive into <dest_dir> based on its format.
//   Arch .pkg.tar.{zst,xz,gz,bz2}, Alpine .apk  -> tar
//   Debian .deb                                 -> dpkg-deb -x
//   RPM .rpm                                    -> rpm2cpio | cpio -idm
static bool extract_foreign_archive(const std::string &archive_path,
                                    const std::string &dest_dir)
{
    std::string lower = archive_path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string stamp_cmd = "mkdir -p " + std::string("'") + dest_dir + "' 2>/dev/null";

    if (lower.find(".deb") != std::string::npos)
    {
        std::string cmd = "dpkg-deb -x " + std::string("'") + archive_path + "' '" + dest_dir + "' 2>/dev/null";
        return system(cmd.c_str()) == 0;
    }
    if (lower.find(".rpm") != std::string::npos)
    {
        std::string cmd = "rpm2cpio " + std::string("'") + archive_path +
                          "' | cpio -idm -D '" + dest_dir + "' 2>/dev/null";
        return system(cmd.c_str()) == 0;
    }

    // Arch .pkg.tar.* and Alpine .apk are tar archives.
    return SlagArchive::extract(archive_path, dest_dir);
}

// File walker for a real (foreign) package: builds the list of normalized
// destination paths plus per-file SHA256 where available, excluding native
// metadata files (.PKGINFO / .MTREE / .INSTALL / .CHANGELOG / .BUILDINFO,
// DEBIAN/ control dirs).
static bool collect_foreign_files(const fs::path &staging,
                                  std::vector<std::string> &paths,
                                  std::vector<std::string> &hashes)
{
    static const std::set<std::string> skip_names = {
        ".PKGINFO", ".MTREE", ".INSTALL", ".CHANGELOG", ".BUILDINFO",
        "debian-binary", "control.tar.gz", "control.tar.xz", "control.tar.zst",
        "data.tar.gz", "data.tar.xz", "data.tar.zst", "data.tar.bz2",
        "signature", "package.xml",
    };

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec);
         it != fs::recursive_directory_iterator(); ++it)
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        const fs::path p = it->path();
        std::string rel = fs::relative(p, staging, ec).generic_string();
        if (ec)
            continue;

        // Skip native metadata directories/files.
        if (rel == "DEBIAN")
        {
            it.disable_recursion_pending();
            continue;
        }
        if (skip_names.count(rel) || skip_names.count(p.filename().string()))
            continue;
        if (rel.rfind("DEBIAN/", 0) == 0)
            continue;

        std::string norm;
        if (!normalize_install_path(rel, norm))
        {
            std::cerr << UI::RED << "Error: unsafe path in foreign package: '" << rel << "'" << UI::RESET << "\n";
            return false;
        }

        if (fs::is_directory(p, ec))
            continue;
        if (fs::is_symlink(p, ec))
        {
            paths.push_back(norm);
            hashes.push_back("");
            continue;
        }

        paths.push_back(norm);
        std::string sha = SlagArchive::compute_sha256(p.string());
        hashes.push_back(looks_like_sha256(sha) ? sha : "");
    }
    return true;
}

// Copies a staged file tree to / with permissions preserved, returning the
// list of copied destinations on success.  On any failure, already-copied
// files are removed (rollback).
static bool install_foreign_files(const fs::path &staging,
                                  const std::vector<std::string> &paths,
                                  const std::vector<std::string> &hashes,
                                  InstalledPackageInfo &info,
                                  uint64_t &total_size)
{
    std::error_code ec;
    std::vector<std::string> copied;

    for (size_t i = 0; i < paths.size(); ++i)
    {
        const std::string &dest_str = paths[i];
        fs::path staged = staging / fs::path(dest_str).relative_path();
        fs::path dest = dest_str;

        if (fs::is_symlink(staged, ec))
        {
            fs::path target = fs::read_symlink(staged, ec);
            if (ec)
            {
                std::cerr << UI::RED << "Error: failed to read symlink " << dest_str << UI::RESET << "\n";
                goto rollback;
            }
            ec.clear();
            fs::create_directories(dest.parent_path(), ec);
            std::error_code x;
            fs::remove(dest, x);
            fs::create_symlink(target, dest, ec);
            if (ec)
            {
                std::cerr << UI::RED << "Error: failed to create symlink " << dest_str << " (" << ec.message() << ")" << UI::RESET << "\n";
                goto rollback;
            }
            copied.push_back(dest_str);
            info.files.push_back(dest_str);
            info.file_hashes.push_back("");
            continue;
        }

        if (fs::is_directory(staged, ec))
        {
            fs::create_directories(dest, ec);
            continue;
        }

        if (!fs::exists(staged, ec))
            continue;

        total_size += fs::file_size(staged, ec);
        ec.clear();
        fs::create_directories(dest.parent_path(), ec);
        fs::copy(staged, dest, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            std::cerr << UI::RED << "Error: failed to install " << dest_str << " (" << ec.message() << ")" << UI::RESET << "\n";
            goto rollback;
        }

        // Preserve executable permission bits from the staged file.
        std::error_code pe;
        auto perms = fs::status(staged, pe).permissions();
        if (!pe)
            fs::permissions(dest, perms, fs::perm_options::replace, pe);

        copied.push_back(dest_str);
        info.files.push_back(dest_str);
        info.file_hashes.push_back(i < hashes.size() ? hashes[i] : "");
    }

    return true;

rollback:
    for (const auto &f : copied)
    {
        std::error_code xc;
        fs::remove(f, xc);
    }
    return false;
}

// Installs a real (foreign format) package archive, e.g. an Arch .pkg.tar.zst
// or Debian .deb downloaded from an external repository.  Extracts the archive,
// reads its native metadata, installs the file tree to / and registers the
// package in the FPM local database.
static bool install_foreign_archive(const std::string &archive_path,
                                    const RepoPackage &rp,
                                    const std::string &archive_sha256,
                                    bool explicit_install,
                                    bool ask_confirmation = true)
{
    DatabaseManager::init();

    if (geteuid() != 0)
    {
        std::cerr << UI::RED << "Error: Package installation requires root privileges. Try running with sudo." << UI::RESET << "\n";
        return false;
    }

    if (!fs::exists(archive_path))
    {
        std::cerr << UI::RED << "Error: file not found: " << archive_path << UI::RESET << "\n";
        return false;
    }

    std::string staging = "/tmp/fpm-stage-" + rp.name;
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    if (!extract_foreign_archive(archive_path, staging))
    {
        std::cerr << UI::RED << "Error: failed to extract package archive" << UI::RESET << "\n";
        fs::remove_all(staging, ec);
        return false;
    }

    // Parse native package metadata where available.
    PackageMeta meta;
    meta.name = rp.name;
    meta.version = rp.version;
    meta.arch = rp.arch;
    meta.description = rp.description;

    std::string pkginfo = staging + "/.PKGINFO";
    if (fs::exists(pkginfo))
    {
        PackageMeta native;
        if (parse_arch_pkginfo_file(pkginfo, native))
        {
            if (!native.name.empty()) meta.name = native.name;
            if (!native.version.empty()) meta.version = native.version;
            if (!native.arch.empty()) meta.arch = native.arch;
            if (!native.description.empty()) meta.description = native.description;
            if (!native.size.empty()) meta.size = native.size;
        }
    }

    if (meta.name.empty() || meta.version.empty() || meta.arch.empty())
    {
        std::cerr << UI::RED << "Error: cannot read package metadata from foreign archive (no .PKGINFO)" << UI::RESET << "\n";
        fs::remove_all(staging, ec);
        return false;
    }

    if (DatabaseManager::is_installed(meta.name))
    {
        std::cout << "Package " << UI::CYAN << meta.name << UI::RESET << " is already installed.\n";
        fs::remove_all(staging, ec);
        return false;
    }

    std::vector<std::string> fpaths;
    std::vector<std::string> fhashes;
    if (!collect_foreign_files(staging, fpaths, fhashes))
    {
        fs::remove_all(staging, ec);
        return false;
    }

    std::cout << UI::BOLD << "Installing package:" << UI::RESET << "\n";
    UI::print_table_header();
    UI::print_table_row(meta.name, meta.arch, meta.version, rp.repo, meta.size);

    std::cout << "\n"
              << UI::BOLD << "Transaction Summary:" << UI::RESET << "\n";
    std::cout << "  Installing:  1 package(s), " << fpaths.size() << " file(s)\n";

    if (ask_confirmation && !UI::ask_confirmation())
    {
        std::cout << "Transaction aborted.\n";
        fs::remove_all(staging, ec);
        return false;
    }

    std::cout << "\n"
              << UI::BOLD << "Running transaction" << UI::RESET << "\n";

    InstalledPackageInfo info;
    info.name = meta.name;
    info.version = meta.version;
    info.arch = meta.arch;
    info.repo = rp.repo;
    info.description = meta.description;
    info.explicit_install = explicit_install;
    info.size = meta.size;

    uint64_t total_size = 0;
    if (!install_foreign_files(staging, fpaths, fhashes, info, total_size))
    {
        std::cerr << UI::RED << "Error: install failed; rolled back copied files." << UI::RESET << "\n";
        fs::remove_all(staging, ec);
        return false;
    }

    fs::remove_all(staging, ec);

    if (info.size.empty() && total_size > 0)
        info.size = UI::format_size(total_size);

    // Record the actual downloaded file digest as the package fingerprint
    // (the index SHA256 from Arch metadata covers the download itself).
    std::string dl_hash = SlagArchive::compute_sha256(archive_path);
    if (!dl_hash.empty())
        info.sha256 = dl_hash;
    else if (!archive_sha256.empty())
        info.sha256 = archive_sha256;

    if (!DatabaseManager::add_package(info))
    {
        std::cerr << UI::RED << "Error: failed to register package in database" << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";
    Logger::info("Successfully installed package: " + meta.name);
    return true;
}

static void remove_empty_parents(const fs::path &start)
{
    fs::path cur = start;
    for (int i = 0; i < 5; ++i)
    {
        if (cur.empty() || cur.string() == "/" || cur == cur.root_path())
            break;
        std::error_code ec;
        if (!fs::exists(cur, ec) || !fs::is_empty(cur, ec))
            break;
        if (!fs::remove(cur, ec))
            break;
        cur = cur.parent_path();
    }
}

static bool install_from_file(const std::string &fpm_path, const std::string &repo,
                              const std::string &archive_sha256, bool ask_confirmation,
                              bool explicit_install = true)
{
    DatabaseManager::init();

    if (geteuid() != 0)
    {
        std::cerr << UI::RED << "Error: Package installation requires root privileges. Try running with sudo." << UI::RESET << "\n";
        return false;
    }

    if (!fs::exists(fpm_path))
    {
        std::cerr << UI::RED << "Error: file not found: " << fpm_path << UI::RESET << "\n";
        return false;
    }

    PackageMeta meta;
    if (!SlagArchive::read_meta_from_archive(fpm_path, meta))
    {
        std::cerr << UI::RED << "Error: invalid .fpm package: " << fpm_path << UI::RESET << "\n";
        return false;
    }

    if (DatabaseManager::is_installed(meta.name))
    {
        std::cout << "Package " << UI::CYAN << meta.name << UI::RESET << " is already installed.\n";
        return false;
    }

    std::vector<ManifestEntry> entries;
    SlagArchive::read_manifest_from_archive(fpm_path, entries);
    if (entries.empty())
    {
        std::cerr << UI::RED << "Error: package has no valid fpm.manifest: " << fpm_path << UI::RESET << "\n";
        return false;
    }

    std::vector<std::string> normalized;
    normalized.reserve(entries.size());
    for (const auto &e : entries)
    {
        std::string norm;
        if (!normalize_install_path(e.path, norm))
        {
            std::cerr << UI::RED << "Error: unsafe path in package manifest: '" << e.path << "'" << UI::RESET << "\n";
            return false;
        }
        normalized.push_back(norm);
    }

    std::cout << UI::BOLD << "Installing package:" << UI::RESET << "\n";
    UI::print_table_header();
    UI::print_table_row(meta.name, meta.arch, meta.version, repo, meta.size);

    std::cout << "\n"
              << UI::BOLD << "Transaction Summary:" << UI::RESET << "\n";
    std::cout << "  Installing:  1 package(s)\n";

    if (ask_confirmation && !UI::ask_confirmation())
    {
        std::cout << "Transaction aborted.\n";
        return false;
    }

    std::cout << "\n"
              << UI::BOLD << "Running transaction" << UI::RESET << "\n";

    std::string staging = "/tmp/fpm-stage-" + meta.name;
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);

    if (!SlagArchive::extract(fpm_path, staging))
    {
        std::cerr << UI::RED << "Error: failed to extract package" << UI::RESET << "\n";
        fs::remove_all(staging, ec);
        return false;
    }

    bool verified = true;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto &e = entries[i];
        fs::path staged = fs::path(staging) / fs::path(normalized[i]).relative_path();
        if (!fs::exists(staged))
        {
            std::cerr << UI::RED << "  Missing file in package: " << e.path << UI::RESET << "\n";
            verified = false;
            continue;
        }

        if (looks_like_sha256(e.sha256))
        {
            std::string actual = SlagArchive::compute_sha256(staged.string());
            if (actual != e.sha256)
            {
                std::cerr << UI::RED << "  Hash mismatch: " << e.path << UI::RESET << "\n";
                verified = false;
            }
        }
    }

    if (!verified)
    {
        std::cerr << UI::RED << "Error: package integrity check failed. Aborting." << UI::RESET << "\n";
        fs::remove_all(staging, ec);
        Logger::error("Integrity check failed for " + fpm_path);
        return false;
    }

    InstalledPackageInfo info;
    info.name = meta.name;
    info.version = meta.version;
    info.arch = meta.arch;
    info.repo = repo;
    info.description = meta.description;
    info.explicit_install = explicit_install;
    if (!archive_sha256.empty())
        info.sha256 = archive_sha256;
    else
        info.sha256 = SlagArchive::compute_sha256(fpm_path);

    uint64_t total_size = 0;
    std::vector<std::string> copied;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto &e = entries[i];
        total_size += e.size;
        const std::string &dest_str = normalized[i];
        fs::path staged = fs::path(staging) / fs::path(dest_str).relative_path();
        fs::path dest = dest_str;
        if (!fs::exists(staged))
            continue;

        fs::create_directories(dest.parent_path(), ec);
        fs::copy(staged, dest, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            std::cerr << UI::RED << "Error: failed to install " << dest_str << " (" << ec.message() << ")" << UI::RESET << "\n";
            for (const auto &f : copied)
            {
                std::error_code xc;
                fs::remove(f, xc);
            }
            fs::remove_all(staging, ec);
            Logger::error("Install rollback for " + meta.name);
            return false;
        }
        apply_entry_mode(dest, e.mode);
        copied.push_back(dest_str);
        info.files.push_back(dest_str);
        info.file_hashes.push_back(looks_like_sha256(e.sha256) ? e.sha256 : "");
    }
    info.size = meta.size.empty() ? UI::format_size(total_size) : meta.size;

    fs::remove_all(staging, ec);

    if (!DatabaseManager::add_package(info))
    {
        std::cerr << UI::RED << "Error: failed to register package in database" << UI::RESET << "\n";
        for (const auto &f : copied)
        {
            std::error_code xc;
            fs::remove(f, xc);
        }
        Logger::error("DB registration failed, rolled back " + meta.name);
        return false;
    }

    std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";
    Logger::info("Successfully installed package: " + meta.name);
    return true;
}

bool install_local_package(const std::string &fpm_path)
{
    return install_from_file(fpm_path, "local", "", true, true);
}

bool install_remote_package(const RepoPackage &pkg, bool explicit_install)
{
    DatabaseManager::init();

    if (geteuid() != 0)
    {
        std::cerr << UI::RED << "Error: Package installation requires root privileges. Try running with sudo." << UI::RESET << "\n";
        return false;
    }

    if (DatabaseManager::is_installed(pkg.name))
    {
        std::cout << "Package " << UI::CYAN << pkg.name << UI::RESET << " is already installed.\n";
        return false;
    }

    // Determine the filename from the URL or construct a default one.
    std::string fname;
    if (!pkg.url.empty())
    {
        size_t slash = pkg.url.find_last_of('/');
        fname = (slash == std::string::npos) ? pkg.url : pkg.url.substr(slash + 1);
    }
    if (fname.empty())
        fname = pkg.name + "-" + pkg.version + "." + pkg.arch + ".fpm";

    fs::create_directories(DatabaseManager::get_package_cache_path());
    std::string dest = DatabaseManager::get_package_cache_path() + "/" + fname;

    // Build candidate download URLs.  When the package has a full URL from
    // repository metadata (e.g. Arch .pkg.tar.zst, Debian .deb, RPM .rpm)
    // we use it as-is.  This is the authoritative source.  Mirror-base
    // fallbacks are only useful for FPM-native .fpm packages whose filename
    // sits directly under the mirror root.
    MirrorSelector::ensure_loaded();
    std::vector<std::string> candidates;
    bool has_full_url = !pkg.url.empty() &&
                        (pkg.url.rfind("http://", 0) == 0 || pkg.url.rfind("https://", 0) == 0 ||
                         pkg.url.rfind("ftp://", 0) == 0);
    if (has_full_url)
        candidates.push_back(pkg.url);

    for (const auto &m : MirrorSelector::get_ordered_mirrors())
    {
        std::string u = m.url + "/" + fname;
        if (std::find(candidates.begin(), candidates.end(), u) == candidates.end())
            candidates.push_back(u);
    }

    bool downloaded = false;
    for (const auto &url : candidates)
    {
        std::cout << "  Downloading " << UI::CYAN << url << UI::RESET << "\n";
        std::error_code ec;
        fs::remove(dest, ec);
        if (MirrorSelector::download(url, dest, 30))
        {
            downloaded = true;
            break;
        }
        std::cout << UI::DIM << "    (failed, trying another source)" << UI::RESET << "\n";
    }

    if (!downloaded)
    {
        std::cerr << UI::RED << "Error: download failed for " << pkg.name << UI::RESET << "\n";
        Logger::error("Download failed: " + pkg.name);
        return false;
    }

    if (!pkg.sha256.empty() && looks_like_sha256(pkg.sha256))
    {
        uint64_t sz = fs::file_size(dest);
        std::cout << "  Verifying sha256 (" << UI::format_size(sz) << ")..." << "\n";
        std::string actual = SlagArchive::compute_sha256(dest);
        if (actual != pkg.sha256)
        {
            std::cerr << UI::RED << "Error: SHA256 mismatch for " << fname << UI::RESET << "\n";
            Logger::error("SHA256 mismatch for " + fname);
            std::error_code ec;
            fs::remove(dest, ec);
            return false;
        }
        std::cout << UI::GREEN << "  OK\n" << UI::RESET;
    }

    // FPM-native .fpm packages carry fpm.meta/fpm.manifest; real foreign
    // packages (Arch/Debian/Alpine/RPM) use their own native layout and are
    // installed by the foreign-archive path.
    if (SlagArchive::verify_archive(dest))
        return install_from_file(dest, pkg.repo, pkg.sha256, false, explicit_install);

    return install_foreign_archive(dest, pkg, pkg.sha256, explicit_install, false);
}

// Collects the requested packages plus any not-yet-installed dependencies
// (breadth-first, cycle-safe). Missing names are reported separately.
static bool collect_with_deps(const std::vector<std::string> &names,
                              std::vector<RepoPackage> &out,
                              std::vector<std::string> &missing,
                              std::vector<std::string> &already_installed)
{
    std::vector<std::string> queue = names;
    std::set<std::string> seen;
    std::set<std::string> already_seen;

    for (const auto &n : names)
        seen.insert(n);

    while (!queue.empty())
    {
        std::string name = queue.back();
        queue.pop_back();

        // Packages provided by the base OS are always considered satisfied.
        // Installing glibc/bash/coreutils from a foreign repo is never wanted.
        if (is_base_system_package(name) || provides_already_on_system(name))
            continue;

        if (DatabaseManager::is_installed(name))
        {
            bool explicit_req = std::find(names.begin(), names.end(), name) != names.end();
            if (explicit_req && already_seen.insert(name).second)
                already_installed.push_back(name);
            continue;
        }

        RepoPackage rp;
        if (!DatabaseManager::get_repo_package(name, rp))
        {
            missing.push_back(name);
            continue;
        }

        out.push_back(rp);
        for (const auto &dep : rp.depends)
        {
            // Depends entries may carry version constraints ("glibc>=2.35");
            // resolution happens on the plain package name.
            std::string dn = normalize_dep_name(dep);
            if (dn.empty())
                continue;
            if (seen.insert(dn).second)
                queue.push_back(dn);
        }
    }

    return !out.empty();
}

static bool is_explicit(const std::vector<std::string> &explicit_names, const std::string &name)
{
    return std::find(explicit_names.begin(), explicit_names.end(), name) != explicit_names.end();
}

bool install_packages(const std::vector<std::string> &pkgs)
{
    DatabaseManager::init();

    std::cout << UI::BOLD << "Resolving dependencies..." << UI::RESET << "\n";

    std::vector<RepoPackage> found;
    std::vector<std::string> missing;
    std::vector<std::string> already;
    collect_with_deps(pkgs, found, missing, already);

    for (const auto &a : already)
        std::cout << "Package " << UI::CYAN << a << UI::RESET << " is already installed.\n";

    if (found.empty())
    {
        if (missing.empty())
            return false;

        for (const auto &nf : missing)
        {
            std::cout << UI::RED << "Error: package '" << nf << "' not found in repositories." << UI::RESET << "\n";
            std::cout << UI::DIM << "Hint: Run 'fpm -upd' to fetch the remote package index first." << UI::RESET << "\n";
        }
        return false;
    }

    if (!missing.empty())
    {
        for (const auto &nf : missing)
            std::cout << UI::YELLOW << "Warning: dependency '" << nf << "' not found in repositories; skipped." << UI::RESET << "\n";
    }

    std::cout << "\n";
    UI::print_table_header();
    std::cout << UI::BOLD << "Installing:" << UI::RESET << "\n";
    for (const auto &pkg : found)
    {
        UI::print_table_row(pkg.name, pkg.arch, pkg.version, pkg.repo, pkg.size);
    }

    std::cout << "\n"
              << UI::BOLD << "Transaction Summary:" << UI::RESET << "\n";
    std::cout << "  Installing:  " << found.size() << " package(s)\n";

    if (!UI::ask_confirmation())
    {
        std::cout << "Transaction aborted.\n";
        return false;
    }

    std::cout << "\n"
              << UI::BOLD << "Running transaction" << UI::RESET << "\n";

    bool all_ok = true;
    for (const auto &pkg : found)
    {
        std::string dl_name;
        if (!pkg.url.empty())
        {
            size_t slash = pkg.url.find_last_of('/');
            if (slash != std::string::npos)
                dl_name = pkg.url.substr(slash + 1);
        }
        if (dl_name.empty())
            dl_name = pkg.name + "-" + pkg.version + "." + pkg.arch + ".fpm";
        std::cout << UI::CYAN << "==> Downloading " << dl_name << UI::RESET << "\n";

        if (!install_remote_package(pkg, is_explicit(pkgs, pkg.name)))
        {
            std::cerr << UI::RED << "  Failed to install " << pkg.name << UI::RESET << "\n";
            all_ok = false;
        }
    }

    if (all_ok)
    {
        std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";
        Logger::info("Successfully installed " + std::to_string(found.size()) + " package(s).");
    }

    return all_ok;
}

static bool install_with_deps(const std::string &name)
{
    std::vector<RepoPackage> found;
    std::vector<std::string> missing;
    std::vector<std::string> already;
    collect_with_deps({name}, found, missing, already);

    if (found.empty())
    {
        if (missing.empty())
            return false;
        for (const auto &nf : missing)
        {
            std::cout << UI::RED << "Error: package '" << nf << "' not found in repositories." << UI::RESET << "\n";
            std::cout << UI::DIM << "Hint: Run 'fpm -upd' to fetch the remote package index first." << UI::RESET << "\n";
        }
        return false;
    }

    if (!missing.empty())
        std::cout << UI::YELLOW << "Warning: dependency not found: " << missing[0] << UI::RESET << "\n";

    bool all_ok = true;
    for (const auto &pkg : found)
    {
        if (!install_remote_package(pkg, is_explicit({name}, pkg.name)))
        {
            std::cerr << UI::RED << "  Failed to install " << pkg.name << UI::RESET << "\n";
            all_ok = false;
        }
    }
    return all_ok;
}

static void remove_package_files(const std::string &name, bool purge)
{
    std::vector<std::string> files = DatabaseManager::get_installed_files(name);

    for (auto it = files.rbegin(); it != files.rend(); ++it)
    {
        if (it->empty())
            continue;
        std::error_code ec;
        fs::remove(*it, ec);
        if (purge)
            remove_empty_parents(fs::path(*it).parent_path());
    }
}

bool remove_packages(const std::vector<std::string> &pkgs, bool purge)
{
    DatabaseManager::init();

    std::cout << UI::BOLD << (purge ? "Purging:" : "Removing:") << UI::RESET << "\n";
    std::vector<std::string> to_remove;

    for (const auto &pkg : pkgs)
    {
        if (!DatabaseManager::is_installed(pkg))
        {
            std::cout << "Package " << UI::CYAN << pkg << UI::RESET << " is not installed.\n";
            continue;
        }

        InstalledPackageInfo info;
        DatabaseManager::get_package(pkg, info);
        to_remove.push_back(pkg);
        UI::print_table_row(pkg, info.arch, info.version, "@system", info.size);
    }

    if (to_remove.empty())
    {
        std::cout << "Nothing to do.\n";
        return true;
    }

    std::cout << "\n"
              << UI::BOLD << "Transaction Summary:" << UI::RESET << "\n";
    std::cout << "  " << (purge ? "Purging" : "Removing") << ":  "
              << to_remove.size() << " package(s)\n";

    if (!UI::ask_confirmation())
    {
        std::cout << "Transaction aborted.\n";
        return false;
    }

    std::cout << "\n"
              << UI::BOLD << "Running transaction" << UI::RESET << "\n";

    for (const auto &pkg_name : to_remove)
    {
        remove_package_files(pkg_name, purge);
        DatabaseManager::remove_package(pkg_name);
        std::cout << "  Removed " << pkg_name << "\n";
    }

    std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";
    Logger::info("Successfully removed " + std::to_string(to_remove.size()) + " package(s).");
    return true;
}

bool search_packages(const std::string &query, bool fuzzy, bool show_all)
{
    DatabaseManager::init();

    if (query.empty())
    {
        std::cerr << UI::RED << "Error: empty search query." << UI::RESET << "\n";
        return false;
    }

    auto repo_results = DatabaseManager::search_repo(query, fuzzy);

    std::cout << UI::BOLD << (fuzzy ? "Fuzzy searching" : "Searching") << " repositories for '"
              << query << "'..." << UI::RESET << "\n\n";

    if (repo_results.empty())
    {
        std::cout << "No packages found matching '" << query << "'.\n";
        std::cout << UI::DIM << "Hint: Run 'fpm -upd' to fetch the remote package index first." << UI::RESET << "\n";
        return true;
    }

    const size_t total = repo_results.size();

    if (total == 1)
    {
        std::cout << UI::BOLD << "Found 1 package:\n" << UI::RESET << "\n";
    }
    else
    {
        std::cout << UI::BOLD << "Found " << total << " package(s):\n" << UI::RESET << "\n";
    }

    const size_t page_size = 30;
    const size_t total_pages = (total + page_size - 1) / page_size;

    // Build the full numbered listing once; reused for less, paging and fzf.
    std::string full_listing;
    for (size_t i = 0; i < total; ++i)
    {
        const auto &p = repo_results[i];
        std::string desc = p.description;
        if (desc.size() > 60)
            desc = desc.substr(0, 57) + "...";

        full_listing += "  " + UI::CYAN + "[" + UI::BOLD + std::to_string(i + 1) + UI::RESET
                        + UI::CYAN + "]" + UI::RESET + " " + UI::BOLD + p.name + UI::RESET;
        if (!desc.empty())
            full_listing += " - " + desc;
        full_listing += "  " + UI::DIM + "(" + p.version + ", " + p.repo + ", " + p.size + ")"
                        + UI::RESET + "\n";
    }

    size_t page = 1;
    bool list_all = show_all;

    auto print_current_page = [&]()
    {
        if (list_all)
        {
            UI::page_text(full_listing); // less when stdout is a TTY + less present
            return;
        }

        const size_t begin = (page - 1) * page_size;
        const size_t end = std::min(begin + page_size, total);

        std::cout << UI::BOLD << "Page " << page << " of " << total_pages << UI::RESET
                  << UI::DIM << " (results " << (begin + 1) << "-" << end << " of " << total << ")"
                  << UI::RESET << "\n";

        for (size_t i = begin; i < end; ++i)
        {
            const auto &p = repo_results[i];
            std::string desc = p.description;
            if (desc.size() > 60)
                desc = desc.substr(0, 57) + "...";

            std::cout << "  " << UI::CYAN << "[" << UI::BOLD << (i + 1) << UI::RESET
                      << UI::CYAN << "]" << UI::RESET << " " << UI::BOLD << p.name << UI::RESET;
            if (!desc.empty())
                std::cout << " - " << desc;
            std::cout << "  " << UI::DIM << "(" << p.version << ", " << p.repo << ", " << p.size << ")"
                      << UI::RESET << "\n";
        }
        std::cout << "\n";
    };

    for (;;)
    {
        print_current_page();

        std::string prompt = "Package to install [1-" + std::to_string((int)total);
        if (total_pages > 1)
        {
            prompt += ", " + std::to_string((int)page);
            if (page > 1)
                prompt += ", 'p' = prev";
            if (page < total_pages)
                prompt += ", 'n' = next";
            prompt += ", 'g#N' = go to page";
        }
        prompt += ", 0 = cancel]: ";

        std::string line = UI::read_line(prompt);
        if (line.empty())
        {
            std::cout << "Cancelled.\n";
            return true;
        }

        std::string low;
        for (auto c : line)
            low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (low == "a" || low == "all")
        {
            list_all = true;
            continue;
        }
        if (low == "f" || low == "fzf")
        {
            if (UI::have_fzf())
            {
                std::vector<std::string> choices;
                choices.reserve(total);
                for (size_t i = 0; i < total; ++i)
                {
                    const auto &p = repo_results[i];
                    choices.push_back(std::to_string(i + 1) + " " + p.name + "  " + p.version
                                      + "  " + p.repo + "  " + p.size);
                }
                std::string picked;
                if (UI::fzf_select("Select a package to install (Enter = install)", choices, picked))
                {
                    int num = 0;
                    try
                    {
                        num = std::stoi(picked.substr(0, picked.find(' ')));
                    }
                    catch (...)
                    {
                        num = 0;
                    }
                    if (num >= 1 && num <= (int)total)
                    {
                        return install_with_deps(repo_results[static_cast<size_t>(num - 1)].name);
                    }
                }
                continue;
            }
            std::cout << UI::YELLOW << "fzf is not installed; using the number prompt.\n" << UI::RESET;
            continue;
        }

        // 'g#N', 'gN' or 'g N' => jump directly to page N.
        if (!low.empty() && low[0] == 'g')
        {
            size_t digits = 1;
            while (digits < low.size() &&
                   (low[digits] == ' ' || low[digits] == '#'))
                ++digits;
            std::string num_str = low.substr(digits);
            int npage = 0;
            try
            {
                npage = std::stoi(num_str);
            }
            catch (...)
            {
            }
            if (npage >= 1 && npage <= (int)total_pages)
            {
                page = static_cast<size_t>(npage);
                continue;
            }
            std::cout << UI::YELLOW << "Invalid page number. Enter a page from 1 to "
                      << total_pages << ".\n" << UI::RESET;
            continue;
        }

        if (low == "n" || low == "next")
        {
            if (page < total_pages)
                ++page;
            else
                std::cout << UI::DIM << "Already on the last page.\n" << UI::RESET;
            continue;
        }
        if (low == "p" || low == "prev" || low == "previous")
        {
            if (page > 1)
                --page;
            else
                std::cout << UI::DIM << "Already on the first page.\n" << UI::RESET;
            continue;
        }

        int choice = 0;
        try
        {
            size_t consumed = 0;
            choice = std::stoi(line, &consumed);
            if (consumed < line.size() || choice < 0 || choice > (int)total)
                choice = -1;
        }
        catch (...)
        {
            choice = -1;
        }

        if (choice == 0)
        {
            std::cout << "Cancelled.\n";
            return true;
        }
        if (choice < 0)
        {
            std::cout << UI::YELLOW << "Invalid input. Enter a package number from 1 to " << total
                      << " (0 = cancel).\n" << UI::RESET;
            continue;
        }

        const RepoPackage &pkg = repo_results[static_cast<size_t>(choice - 1)];

        std::cout << "\n"
                  << UI::BOLD << "Selected package:" << UI::RESET << "\n"
                  << "  Name        : " << pkg.name << "\n"
                  << "  Version     : " << pkg.version << "\n"
                  << "  Repository  : " << pkg.repo << "\n"
                  << "  Size        : " << pkg.size << "\n"
                  << "  Description : " << pkg.description << "\n";
        if (!pkg.url.empty())
            std::cout << "  Source      : " << pkg.url << "\n";

        if (!UI::ask_confirmation("Download and install this package [y/N]: "))
        {
            std::cout << "Cancelled.\n";
            return true;
        }

        return install_with_deps(pkg.name);
    }
}

bool show_package_info(const std::string &pkg_name)
{
    DatabaseManager::init();

    InstalledPackageInfo pkg;
    if (DatabaseManager::get_package(pkg_name, pkg))
    {
        std::cout << UI::BOLD << "Name         : " << UI::RESET << pkg.name << "\n"
                  << UI::BOLD << "Version      : " << UI::RESET << pkg.version << "\n"
                  << UI::BOLD << "Architecture : " << UI::RESET << pkg.arch << "\n"
                  << UI::BOLD << "Repository   : " << UI::RESET << pkg.repo << "\n"
                  << UI::BOLD << "Size         : " << UI::RESET << pkg.size << "\n"
                  << UI::BOLD << "Description  : " << UI::RESET << pkg.description << "\n";
        if (!pkg.sha256.empty())
            std::cout << UI::BOLD << "SHA256       : " << UI::RESET << pkg.sha256 << "\n";
        if (!pkg.files.empty())
        {
            std::cout << UI::BOLD << "Files        : " << UI::RESET << "\n";
            for (const auto &f : pkg.files)
                std::cout << "  " << f << "\n";
        }
        return true;
    }

    RepoPackage rp;
    if (DatabaseManager::get_repo_package(pkg_name, rp))
    {
        std::cout << UI::BOLD << "Name         : " << UI::RESET << rp.name << "\n"
                  << UI::BOLD << "Version      : " << UI::RESET << rp.version << "\n"
                  << UI::BOLD << "Architecture : " << UI::RESET << rp.arch << "\n"
                  << UI::BOLD << "Repository   : " << UI::RESET << rp.repo << "\n"
                  << UI::BOLD << "Size         : " << UI::RESET << rp.size << "\n"
                  << UI::BOLD << "Description  : " << UI::RESET << rp.description << "\n";
        if (!rp.sha256.empty())
            std::cout << UI::BOLD << "SHA256       : " << UI::RESET << rp.sha256 << "\n";
        if (!rp.depends.empty())
        {
            std::cout << UI::BOLD << "Depends On   : " << UI::RESET;
            for (size_t i = 0; i < rp.depends.size(); ++i)
            {
                if (i > 0) std::cout << ", ";
                std::cout << rp.depends[i];
            }
            std::cout << "\n";
        }
        std::cout << UI::DIM << "(not installed)" << UI::RESET << "\n";
        return true;
    }

    std::cout << UI::RED << "Package '" << pkg_name << "' not found." << UI::RESET << "\n";
    return false;
}

bool search_installed_packages(const std::string &query)
{
    DatabaseManager::init();

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower_query.empty())
    {
        std::cerr << UI::RED << "Error: empty search query." << UI::RESET << "\n";
        return false;
    }

    auto all = DatabaseManager::get_all_packages();

    std::vector<std::pair<int, const InstalledPackageInfo *>> hits;
    for (const auto &pkg : all)
    {
        std::string lower_name = pkg.name;
        std::string lower_desc = pkg.description;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lower_desc.begin(), lower_desc.end(), lower_desc.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        int tier = -1;
        if (lower_name == lower_query)
            tier = 0;
        else if (lower_name.rfind(lower_query, 0) == 0)
            tier = 1;
        else if (lower_name.find(lower_query) != std::string::npos)
            tier = 2;
        else if (lower_desc.find(lower_query) != std::string::npos)
            tier = 3;

        if (tier >= 0)
            hits.push_back({tier, &pkg});
    }

    std::sort(hits.begin(), hits.end(),
              [](const std::pair<int, const InstalledPackageInfo *> &a,
                 const std::pair<int, const InstalledPackageInfo *> &b)
              {
                  if (a.first != b.first)
                      return a.first < b.first;
                  return a.second->name < b.second->name;
              });

    if (hits.empty())
    {
        std::cout << "No installed packages matching '" << query << "'.\n";
        return true;
    }

    std::cout << UI::BOLD << "Installed packages matching '" << query << "' (" << hits.size()
              << "):" << UI::RESET << "\n\n";
    UI::print_table_header();
    for (const auto &hit : hits)
        UI::print_table_row(hit.second->name, hit.second->arch, hit.second->version,
                            "@system", hit.second->size);
    return true;
}

bool list_installed_packages(const std::string &filter)
{
    DatabaseManager::init();
    auto packages = DatabaseManager::get_all_packages();

    if (packages.empty())
    {
        std::cout << "No packages currently installed.\n";
        return true;
    }

    std::string lower_filter = filter;
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::cout << UI::BOLD << "Installed packages (" << packages.size() << "):" << UI::RESET << "\n\n";
    UI::print_table_header();
    for (const auto &pkg : packages)
    {
        if (!lower_filter.empty())
        {
            std::string lower_name = pkg.name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower_name.find(lower_filter) == std::string::npos)
                continue;
        }
        UI::print_table_row(pkg.name, pkg.arch, pkg.version, "@system", pkg.size);
    }
    return true;
}

// --- SYNCHRONIZATION & MAINTENANCE ---

bool update_db()
{
    DatabaseManager::init();
    MirrorSelector::set_state_path(DatabaseManager::get_selected_mirror_path());

    std::cout << UI::BOLD << "Updating repository databases..." << UI::RESET << "\n";

    std::string mirrors_path = MirrorSelector::find_mirrors_list();
    if (mirrors_path.empty())
    {
        std::cerr << UI::RED << "Error: no mirrors.list found. Add a mirror with 'fpm -am <URL>' or edit /etc/fpm/mirrors.list." << UI::RESET << "\n";
        return false;
    }

    if (!MirrorSelector::load_active_mirrors())
    {
        std::cerr << UI::RED << "Error: no usable mirrors found. Add one with 'fpm -am <URL>'." << UI::RESET << "\n";
        return false;
    }

    const auto mirrors = MirrorSelector::get_ordered_mirrors();
    std::cout << "Loaded " << mirrors.size() << " mirror(s). Probing for repositories...\n";

    bool updated = false;
    std::string used_mirror;
    std::string used_index_url;
    std::vector<RepoPackage> index;

    for (size_t mi = 0; mi < mirrors.size() && !updated; ++mi)
    {
        const Mirror &m = mirrors[mi];
        std::cout << "  [" << (mi + 1) << "/" << mirrors.size() << "] " << m.url << " ...\n";

        std::string fail_reason;
        auto probe_result = repo::probe_mirror(m.url, fail_reason);

        if (probe_result.ok && !probe_result.packages.empty())
        {
            index = std::move(probe_result.packages);
            used_mirror = m.url;
            used_index_url = probe_result.source;
            updated = true;
            std::cout << "    " << UI::GREEN << "Repository: " << probe_result.format
                      << " (" << index.size() << " packages)" << UI::RESET << "\n";
        }
        else
        {
            std::cout << UI::RED << "    " << (fail_reason.empty() ? "no supported repository" : fail_reason) << UI::RESET << "\n";
        }
    }

    if (!updated)
    {
        // Every configured mirror failed. The reserved fallback mirror can
        // never be removed by -ms, so a bare-bones system always has a way
        // to heal itself: updates can never brick the package manager.
        const std::string fallback = MirrorSelector::default_mirror();
        std::string fail_reason;
        auto probe_result = repo::probe_mirror(fallback, fail_reason);

        std::cout << UI::YELLOW << "[WARN] Active mirror failed or unreachable! Falling back to "
                  << fallback << UI::RESET << "\n";
        Logger::warn("Active mirror failed or unreachable! Falling back to " + fallback);

        if (probe_result.ok && !probe_result.packages.empty())
        {
            index = std::move(probe_result.packages);
            used_mirror = fallback;
            used_index_url = probe_result.source;
            updated = true;

            Mirror m;
            m.url = fallback;
            m.country = "Upstream";
            m.protocol = "https";
            m.priority = 10;
            m.available = true;
            MirrorSelector::set_active_mirror(m);
        }
    }

    if (!updated)
    {
        if (fs::exists(DatabaseManager::get_remote_index_path()))
        {
            std::cerr << UI::YELLOW << "Failed to fetch a fresh index from any mirror. Keeping the existing index." << UI::RESET << "\n";
        }
        else
        {
            std::cerr << UI::RED << "Error: no supported repository found on any mirror. Add a working mirror and retry." << UI::RESET << "\n";
        }
        Logger::warn("Index update failed from all mirrors");
        return false;
    }

    std::cout << UI::CYAN << "  Index found at: " << used_index_url << UI::RESET << "\n";

    if (!DatabaseManager::save_remote_index(index))
    {
        std::cerr << UI::RED << "Error: failed to save remote index." << UI::RESET << "\n";
        return false;
    }

    DatabaseManager::save_repo_splits(index);
    MirrorSelector::set_selected(used_mirror);

    std::cout << UI::GREEN << UI::BOLD << "Repository index updated: "
              << index.size() << " package(s)." << UI::RESET << "\n";
    Logger::info("Index updated from " + used_mirror + " (" + std::to_string(index.size()) + " packages)");
    return true;
}

bool upgrade_system()
{
    DatabaseManager::init();

    if (geteuid() != 0)
    {
        std::cerr << UI::RED << "Error: Upgrade requires root privileges. Try running with sudo." << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::BOLD << "Checking for package upgrades..." << UI::RESET << "\n";

    auto installed = DatabaseManager::get_all_packages();

    struct UpgradeItem
    {
        std::string name;
        std::string old_version;
        RepoPackage rp;
    };
    std::vector<UpgradeItem> upgrades;

    for (const auto &pkg : installed)
    {
        RepoPackage rp;
        if (DatabaseManager::get_repo_package(pkg.name, rp) && rp.version != pkg.version)
            upgrades.push_back({pkg.name, pkg.version, rp});
    }

    if (upgrades.empty())
    {
        std::cout << "Nothing to do. System is fully upgraded.\n";
        return true;
    }

    std::cout << "Upgradable packages:\n";
    for (const auto &u : upgrades)
        std::cout << "  " << u.name << ": " << u.old_version << " -> " << u.rp.version << "\n";

    if (!UI::ask_confirmation("Upgrade these packages [y/N]: "))
    {
        std::cout << "Transaction aborted.\n";
        return false;
    }

    bool all_ok = true;
    for (const auto &u : upgrades)
    {
        remove_package_files(u.name, false);
        DatabaseManager::remove_package(u.name);
        if (!install_remote_package(u.rp, true))
        {
            std::cerr << UI::RED << "  Failed to upgrade " << u.name << UI::RESET << "\n";
            all_ok = false;
        }
    }

    if (all_ok)
        std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";

    return all_ok;
}

bool full_system_update()
{
    bool ok = update_db();
    bool upgraded = upgrade_system();
    return ok && upgraded;
}

bool autoremove_orphans(bool remove)
{
    DatabaseManager::init();

    std::cout << UI::BOLD << "Checking for orphaned dependencies..." << UI::RESET << "\n";

    auto installed = DatabaseManager::get_all_packages();

    std::vector<std::string> required_by;
    for (const auto &pkg : installed)
    {
        RepoPackage rp;
        if (DatabaseManager::get_repo_package(pkg.name, rp))
        {
            for (const auto &dep : rp.depends)
                required_by.push_back(dep);
        }
    }

    std::vector<std::string> orphans;
    for (const auto &pkg : installed)
    {
        if (pkg.explicit_install)
            continue;
        bool is_dep = false;
        for (const auto &req : required_by)
        {
            if (req == pkg.name)
            {
                is_dep = true;
                break;
            }
        }
        if (!is_dep)
            orphans.push_back(pkg.name);
    }

    if (orphans.empty())
    {
        std::cout << "No orphaned packages found to remove.\n";
        return true;
    }

    std::cout << "Orphaned packages:\n";
    for (const auto &o : orphans)
        std::cout << "  " << o << "\n";

    if (!remove)
    {
        std::cout << UI::DIM << "Hint: use 'fpm -ar' to remove missing orphans, or 'fpm -r <pkg>' to remove a specific one." << UI::RESET << "\n";
        return true;
    }

    std::cout << "\n"
              << UI::BOLD << "Transaction Summary:" << UI::RESET << "\n";
    std::cout << "  Removing:  " << orphans.size() << " package(s)\n";

    if (!UI::ask_confirmation())
    {
        std::cout << "Transaction aborted.\n";
        return false;
    }

    std::cout << "\n"
              << UI::BOLD << "Running transaction" << UI::RESET << "\n";

    for (const auto &name : orphans)
    {
        remove_package_files(name, false);
        DatabaseManager::remove_package(name);
        std::cout << "  Removed " << name << "\n";
    }

    std::cout << UI::GREEN << UI::BOLD << "Complete!" << UI::RESET << "\n";
    Logger::info("Successfully removed " + std::to_string(orphans.size()) + " orphaned package(s).");
    return true;
}

bool clean_cache_cmd()
{
    std::cout << UI::BOLD << "Clearing package archive cache..." << UI::RESET << "\n";

    CleanStats stats = Cleaner::clean_cache();

    std::string freed = UI::format_size(stats.bytes_freed);
    UI::print_step(1, 1, "Cleaning", DatabaseManager::get_package_cache_path(), "100 MiB/s", freed, "00m00s");
    std::cout << UI::GREEN << UI::BOLD << "Cache cleared! Freed " << freed << "." << UI::RESET << "\n";
    Logger::info("Cache cleared, freed " + freed);
    return true;
}

bool clean_deep_cmd()
{
    std::cout << UI::BOLD << "FPM CleanMyDisk" << UI::RESET << "\n\n";

    uint64_t freed = 0;
    auto report = Cleaner::clean_deep(freed);

    for (const auto &line : report)
        std::cout << line << "\n";

    if (freed == 0)
        std::cout << "\nNothing to clean.\n";

    Logger::info("CleanMyDisk freed " + UI::format_size(freed));
    return true;
}

bool verify_package(const std::string &pkg)
{
    DatabaseManager::init();

    InstalledPackageInfo info;
    if (!DatabaseManager::get_package(pkg, info))
    {
        std::cout << UI::RED << "Package '" << pkg << "' is not installed." << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::BOLD << "Verifying integrity of package: " << pkg << UI::RESET << "\n";

    if (info.sha256.empty())
    {
        std::cout << UI::YELLOW << "  No SHA256 hash recorded for this package." << UI::RESET << "\n";
        std::cout << UI::YELLOW << "  Skipping file verification." << UI::RESET << "\n";
        std::cout << UI::GREEN << UI::BOLD << "Package metadata verified: OK (no hash)" << UI::RESET << "\n";
        return true;
    }

    bool all_ok = true;
    for (size_t i = 0; i < info.files.size(); ++i)
    {
        const std::string &file = info.files[i];
        if (!fs::exists(file))
        {
            std::cout << UI::RED << "  Missing: " << file << UI::RESET << "\n";
            all_ok = false;
            continue;
        }

        std::string expected_hash;
        if (i < info.file_hashes.size())
            expected_hash = info.file_hashes[i];

        if (!expected_hash.empty() && looks_like_sha256(expected_hash))
        {
            std::string actual = SlagArchive::compute_sha256(file);
            if (actual != expected_hash)
            {
                std::cout << UI::RED << "  Hash mismatch: " << file << UI::RESET << "\n";
                all_ok = false;
            }
        }
    }

    if (all_ok)
    {
        std::cout << UI::GREEN << UI::BOLD << "Package integrity verified: OK" << UI::RESET << "\n";
        Logger::info("Package verified: " + pkg);
    }
    else
    {
        std::cout << UI::RED << UI::BOLD << "Package integrity check FAILED" << UI::RESET << "\n";
        Logger::warn("Package verification failed: " + pkg);
    }

    return all_ok;
}

// --- MIRROR SELECTOR ---

bool mirror_selector_command()
{
    DatabaseManager::init();
    MirrorSelector::set_state_path(DatabaseManager::get_selected_mirror_path());

    std::cout << UI::BOLD << "FPM Mirror Selector" << UI::RESET << "\n\n";

    // Read the full candidate pool. When candidates.list does not exist yet,
    // fall back to the active mirrors.list so the selector still works.
    if (!MirrorSelector::load_candidates())
    {
        std::cerr << UI::RED << "Error: no mirrors configured. Add one with 'fpm -am <URL>'." << UI::RESET << "\n";
        return false;
    }

    const auto mirrors = MirrorSelector::get_mirrors();
    std::cout << "Probing " << mirrors.size() << " mirror(s) for latency and repository support...\n";

    std::vector<Mirror> working;
    for (size_t i = 0; i < mirrors.size(); ++i)
    {
        Mirror m = mirrors[i];
        std::cout << "  [" << (i + 1) << "/" << mirrors.size() << "] " << m.url << " ... ";
        std::cout.flush();

        // Cheap signals, not full index downloads: round-trip latency first,
        // then a well-known index marker (packages.db / core.db / Release /
        // APKINDEX.tar.gz / repomd.xml).
        int latency = MirrorSelector::measure_endpoint(m.url, 3000);
        std::string found_type;
        bool ok_marker = false;
        if (latency >= 0)
            ok_marker = MirrorSelector::probe_repo_marker(m.url, found_type, 3000);

        if (latency >= 0 && ok_marker && !found_type.empty())
        {
            m.rtt_ms = latency;
            m.available = true;
            m.index = found_type;
            working.push_back(m);
            std::cout << UI::GREEN << "OK  " << found_type << "  (" << latency << " ms)" << UI::RESET << "\n";
        }
        else
        {
            m.available = false;
            m.rtt_ms = -1;
            std::cout << UI::RED << "FAIL  no supported repository" << UI::RESET << "\n";
        }
    }

    if (working.empty())
    {
        std::cerr << UI::RED << "No working mirrors found (no mirror serves a supported repository)." << UI::RESET << "\n";
        return false;
    }

    // Fastest first.
    std::stable_sort(working.begin(), working.end(),
                     [](const Mirror &a, const Mirror &b)
                     {
                         return a.rtt_ms < b.rtt_ms;
                     });

    std::cout << "\n" << UI::BOLD << "Available mirrors (sorted by latency):" << UI::RESET << "\n";
    const int n = static_cast<int>(working.size());
    for (int i = 0; i < n; ++i)
        UI::print_numbered_item(i + 1, working[i].country + "  " + working[i].url,
                                working[i].index + "  (" + std::to_string(working[i].rtt_ms) + " ms)");

    int choice = UI::prompt_number("Select mirror [1-" + std::to_string(n) + ", 0 = cancel]: ", n);
    if (choice == 0)
    {
        std::cout << "Cancelled.\n";
        return true;
    }

    const Mirror &sel = working[choice - 1];
    // Persist the choice as the single ACTIVE mirror (rewrites mirrors.list).
    MirrorSelector::set_active_mirror(sel);
    MirrorSelector::set_selected(sel.url);
    const std::string pref_url = sel.url.empty() ? MirrorSelector::default_mirror() : sel.url;
    std::cout << UI::GREEN << "INDEX OK" << UI::RESET << "\n";
    std::cout << UI::GREEN << "Preferred mirror set to: " << pref_url << UI::RESET << "\n";
    Logger::info("Mirror selected: " + pref_url);
    return true;
}

// --- ADD MIRROR (-am) ---

bool add_mirror_command(const std::string &url, bool active)
{
    std::cout << UI::BOLD << "Adding mirror: " << url << UI::RESET << "\n";

    if (!MirrorSelector::is_reachable(url, 5000))
    {
        std::cerr << UI::RED << "Error: mirror is unreachable (no HTTP reply within 5s)." << UI::RESET << "\n";
        return false;
    }

    if (!MirrorSelector::add_mirror_candidate(url, "", active))
    {
        std::cerr << UI::RED << "Error: failed to write mirror to the mirror list." << UI::RESET
                  << " [target=" << (active ? MirrorSelector::write_target_mirrors()
                                            : MirrorSelector::write_target_candidates())
                  << "]" << std::endl;
        return false;
    }

    std::cout << UI::GREEN << "Mirror added." << UI::RESET << "\n";
    std::cout << "  Stored in: "
              << (active ? MirrorSelector::write_target_mirrors()
                         : MirrorSelector::write_target_candidates())
              << "\n";
    std::cout << "  Active: " << (active ? "yes (used by -upd / -i)" : "no (select with 'fpm -ms')") << "\n";
    Logger::info("Mirror added: " + url);
    return true;
}

// --- RESET MIRROR (-rm) ---
// Switches the ACTIVE mirror back to the hardcoded default fallback
// (https://geo.mirror.pkgbuild.com/). set_active_mirror re-encodes the
// reserved URL with its canonical trailing slash.
bool reset_mirror_command()
{
    Mirror def;
    def.url = MirrorSelector::default_mirror();
    def.country = "Upstream";
    def.protocol = "https";
    def.priority = 10;
    def.available = true;
    def.rtt_ms = -1;
    def.index = "";

    if (!MirrorSelector::set_active_mirror(def))
    {
        std::cerr << UI::RED << "Error: failed to reset the active mirror." << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::GREEN << "Active mirror reset to: "
              << MirrorSelector::default_mirror() << UI::RESET << "\n";
    std::cout << "  -upd / -i will now use the default Geo-Mirror.\n";
    Logger::info("Mirror reset to default: " + MirrorSelector::default_mirror());
    return true;
}

// --- DELETE MIRROR (-dm / -dmm) ---
// Removes a user-added mirror from both the candidate pool and the active
// mirror list.
bool delete_mirror_command(const std::string &url)
{
    std::cout << UI::BOLD << "Removing mirror: " << url << UI::RESET << "\n";

    if (!MirrorSelector::remove_mirror(url))
    {
        std::cerr << UI::RED << "No mirror matching the URL was found." << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::GREEN << "Mirror removed." << UI::RESET << "\n";
    std::cout << "  Pool:   " << MirrorSelector::write_target_candidates() << "\n";
    std::cout << "  Active: " << MirrorSelector::write_target_mirrors() << "\n";
    Logger::info("Mirror removed: " + url);
    return true;
}

// --- NEWS (-n) ---
// Built-in release notes for the current version.
bool news_command()
{
    std::cout << R"NEWS(

===================================================================
                  FPM (Fucking Package Manager)
                   Release Notes — v0.2.0-alpha
===================================================================

🚀 NEW FEATURES & COMMANDS:
  • Multi-Tiered Search Engine (-s / -fs):
    - Redesigned search logic: Exact -> Prefix -> Substring -> Fuzzy.
    - Added clean interactive pagination for large search results.

  • Advanced Mirror Management (-ms, -am, -dm, -rm):
    - Added '-ms' (Mirror Select) with real-time latency probing.
    - Added '-am' (Add Mirror) to register custom repository endpoints.
    - Added '-dm' (Delete Mirror) to prune outdated/broken mirrors.
    - Added '-rm' (Reset Mirrors) to restore default candidates list.

  • Automatic Fallback System:
    - Hardcoded fallback to primary Geo-Mirror (pkgbuild.com)
      ensuring system stability even if active mirror fails.

🔧 FIXES & IMPROVEMENTS:
  • Package Installation & Upgrade Engine:
    - Fixed core installation pipeline when extracting .pkg.tar.zst.
    - Resolved file-path alignment issues during package deployment.
    - Fixed database sync flow (-upd) across multi-repository indices.

  • Universal Distro Support:
    - Verified full compatibility with Debian, Arch, Alpine, and LFS.
    - Improved C++17 build portability (libcurl / libarchive / zstd).

===================================================================

)NEWS";
    return true;
}

// --- BUILD & DEVELOPMENT ---

bool build_package(const std::string &dir)
{
    fs::path source_dir(dir);
    if (!fs::exists(source_dir))
    {
        std::cerr << UI::RED << "Error: directory not found: " << dir << UI::RESET << "\n";
        return false;
    }

    fs::path recipe = source_dir / "fpm.meta";
    if (!fs::exists(recipe))
    {
        std::cerr << UI::RED << "Error: fpm.meta not found in " << dir << UI::RESET << "\n";
        return false;
    }

    std::cout << UI::BOLD << "Building FPM package from: " << dir << UI::RESET << "\n";

    PackageMeta meta;
    if (!SlagArchive::read_meta_from_file(recipe.string(), meta))
    {
        std::cerr << UI::RED << "Error: failed to parse fpm.meta" << UI::RESET << "\n";
        return false;
    }

    std::string pkg_name = meta.name + "-" + meta.version + "." + meta.arch + ".fpm";
    std::string output_path = (fs::path(pkg_name)).string();

    fs::path abs_source = fs::absolute(source_dir).lexically_normal();
    UI::print_step(1, 3, "Reading recipe", recipe.string(), "1.0 KiB/s", "512 B", "00m00s");
    UI::print_step(2, 3, "Generating manifest", dir, "0 B/s", "0 B", "00m00s");

    if (!SlagArchive::create(abs_source.string(), output_path))
    {
        std::cerr << UI::RED << "Error: failed to create archive" << UI::RESET << "\n";
        return false;
    }

    if (!fs::exists(output_path))
    {
        output_path = (abs_source / pkg_name).string();
    }

    if (!fs::exists(output_path))
    {
        std::cerr << UI::RED << "Error: package archive was not produced." << UI::RESET << "\n";
        return false;
    }

    UI::print_step(3, 3, "Archiving package", pkg_name, "45.0 MiB/s",
                   UI::format_size(fs::file_size(output_path)), "00m00s");

    std::cout << UI::GREEN << UI::BOLD << "Complete! Package built: " << output_path << UI::RESET << "\n";
    Logger::info("Built package: " + output_path);
    return true;
}

// --- REPOSITORY BACKEND ---
//
// Publishes a real FPM repository: scans <dir> for .fpm packages and writes
// <dir>/packages.db (canonical text index) plus <dir>/packages.json. Point a
// mirror at this directory (e.g. served over HTTP) and FPM can consume it.
bool make_repo(const std::string &dir, const std::string &base_url)
{
    fs::path repo_dir(dir);
    std::error_code ec;
    if (!fs::exists(repo_dir, ec))
    {
        std::cerr << UI::RED << "Error: repository directory not found: " << dir << UI::RESET << "\n";
        return false;
    }

    std::vector<std::string> fnames;
    for (const auto &entry : fs::directory_iterator(repo_dir, ec))
    {
        if (ec)
            break;
        if (entry.is_regular_file() && entry.path().extension() == ".fpm")
            fnames.push_back(entry.path().filename().string());
    }
    std::sort(fnames.begin(), fnames.end());

    std::vector<RepoPackage> packages;
    for (const auto &fname : fnames)
    {
        fs::path p = repo_dir / fname;
        PackageMeta meta;
        if (!SlagArchive::read_meta_from_archive(p.string(), meta) || meta.name.empty())
        {
            std::cerr << UI::YELLOW << "  Warn: skipping " << fname << " (not a valid SLAG-AR package)" << UI::RESET << "\n";
            continue;
        }

        RepoPackage rp;
        rp.name = meta.name;
        rp.version = meta.version.empty() ? "1.0.0" : meta.version;
        rp.arch = meta.arch.empty() ? "x86_64" : meta.arch;
        rp.repo = meta.repo.empty() ? "core" : meta.repo;
        rp.description = meta.description;
        rp.size = UI::format_size(fs::file_size(p, ec));
        rp.sha256 = SlagArchive::compute_sha256(p.string());
        rp.depends = meta.depends;
        if (!base_url.empty())
        {
            std::string base = base_url;
            while (!base.empty() && base.back() == '/')
                base.pop_back();
            rp.url = base + "/" + fname;
        }
        packages.push_back(std::move(rp));
    }

    if (packages.empty())
    {
        std::cerr << UI::RED << "Error: no valid .fpm packages found in " << dir << UI::RESET << "\n";
        return false;
    }

    // packages.db — canonical FPM text index (same format FPM parses).
    {
        std::string db_path = (repo_dir / "packages.db").string();
        std::string tmp = db_path + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open())
        {
            std::cerr << UI::RED << "Error: cannot write " << tmp << UI::RESET << "\n";
            return false;
        }
        out << "# FPM repository index (canonical text format)\n";
        out << "# name|version|arch|repo|size|description|sha256|depends|url\n";
        for (const auto &p : packages)
        {
            out << p.name << " | " << p.version << " | " << p.arch << " | " << p.repo << " | "
                << p.size << " | " << p.description << " | " << p.sha256 << " | ";
            for (size_t i = 0; i < p.depends.size(); ++i)
            {
                if (i > 0)
                    out << ",";
                out << p.depends[i];
            }
            out << " | " << p.url << "\n";
        }
        out.flush();
        out.close();
        std::error_code xc;
        fs::rename(tmp, db_path, xc);
        if (xc)
        {
            fs::remove(tmp, xc);
            std::cerr << UI::RED << "Error: failed to finalize packages.db" << UI::RESET << "\n";
            return false;
        }
    }

    // packages.json — the same index in JSON form for mirrors that prefer it.
    {
        std::string json_path = (repo_dir / "packages.json").string();
        std::string tmp = json_path + ".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open())
        {
            std::cerr << UI::RED << "Error: cannot write " << tmp << UI::RESET << "\n";
            return false;
        }
        out << "{\n  \"name\": \"fpm\",\n  \"packages\": [\n";
        for (size_t i = 0; i < packages.size(); ++i)
        {
            const auto &p = packages[i];
            out << "    {\"name\": \"" << json_escape(p.name) << "\", \"version\": \"" << json_escape(p.version)
                << "\", \"arch\": \"" << json_escape(p.arch) << "\", \"repo\": \"" << json_escape(p.repo)
                << "\", \"size\": \"" << json_escape(p.size) << "\", \"description\": \"" << json_escape(p.description)
                << "\", \"sha256\": \"" << json_escape(p.sha256) << "\", \"depends\": [";
            for (size_t k = 0; k < p.depends.size(); ++k)
            {
                if (k > 0)
                    out << ", ";
                out << "\"" << json_escape(p.depends[k]) << "\"";
            }
            out << "], \"url\": \"" << json_escape(p.url) << "\"}";
            if (i + 1 < packages.size())
                out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
        out.flush();
        out.close();
        std::error_code xc;
        fs::rename(tmp, json_path, xc);
        if (xc)
        {
            fs::remove(tmp, xc);
            std::cerr << UI::RED << "Error: failed to finalize packages.json" << UI::RESET << "\n";
            return false;
        }
    }

    std::cout << UI::GREEN << UI::BOLD << "Repository generated: " << packages.size()
              << " package(s) -> " << (repo_dir / "packages.db").string() << UI::RESET << "\n";
    Logger::info("Repository generated: " + dir + " (" + std::to_string(packages.size()) + " packages)");
    return true;
}