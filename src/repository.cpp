#include "repository.hpp"
#include "mirror_selector.hpp"
#include "database.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace repo
{
    static std::string trim(const std::string &s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    std::string normalize_dep_name(std::string dep)
    {
        dep = trim(dep);
        size_t p = dep.find_first_of(">=<");
        if (p != std::string::npos)
            dep = dep.substr(0, p);
        return trim(dep);
    }

    bool looks_like_sha256(const std::string &s)
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

    std::string deptool_name()
    {
        return "pacman";
    }

    // -----------------------------------------------------------------------
    // FPM-native adapter
    // -----------------------------------------------------------------------
    //
    // Probes for the proprietary FPM index (packages.db / packages.json) and
    // parses the canonical pipe-delimited text or JSON format.  This is the
    // *only* format that can carry FPM-specific SHA256 hashes of .fpm files
    // and should be listed first so that genuine FPM mirrors are preferred.

    static std::vector<std::string> fpm_index_candidates()
    {
        return {
            "/packages.db",
            "/packages.json",
            "/index.db",
            "/index.json",
        };
    }

    // Pipe-delimited FPM text index parser.
    static std::vector<RepoPackage> parse_fpm_text(const std::string &content,
                                                   const std::string &base)
    {
        std::vector<RepoPackage> result;
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            RepoPackage pkg;
            std::istringstream ss(line);
            std::string token;
            std::vector<std::string> fields;
            while (std::getline(ss, token, '|'))
                fields.push_back(trim(token));

            if (fields.size() < 5)
                continue;

            pkg.name = fields[0];
            if (fields.size() >= 2) pkg.version = fields[1];
            if (fields.size() >= 3) pkg.arch = fields[2];
            if (fields.size() >= 4) pkg.repo = fields[3];
            if (fields.size() >= 5) pkg.size = fields[4];
            if (fields.size() >= 6) pkg.description = fields[5];
            if (fields.size() >= 7) pkg.sha256 = fields[6];
            if (fields.size() >= 8)
            {
                std::istringstream ds(fields[7]);
                std::string dep;
                while (std::getline(ds, dep, ','))
                {
                    std::string dn = normalize_dep_name(dep);
                    if (!dn.empty())
                        pkg.depends.push_back(dn);
                }
            }
            if (fields.size() >= 9)
                pkg.url = fields[8];

            if (pkg.url.empty() && !base.empty() && !pkg.name.empty())
                pkg.url = base + "/" + pkg.name + "-" + pkg.version +
                          "." + pkg.arch + ".fpm";

            if (!pkg.name.empty() && !pkg.version.empty() && !pkg.arch.empty())
                result.push_back(std::move(pkg));
        }
        return result;
    }

    // Minimal JSON FPM index parser (no external dependency).
    static std::vector<RepoPackage> parse_fpm_json(const std::string &content,
                                                   const std::string &base)
    {
        std::vector<RepoPackage> result;

        // Find the "packages" array or treat root as a single object / array.
        // We do a lightweight extraction instead of a full JSON parser because
        // FPM's own JSON format is well-structured and predictable.
        auto find_arr = [&](const std::string &key) -> size_t
        {
            std::string needle = "\"" + key + "\"";
            size_t p = content.find(needle);
            if (p == std::string::npos)
                return std::string::npos;
            p = content.find('[', p);
            return p;
        };

        size_t arr_start = find_arr("packages");
        if (arr_start == std::string::npos)
            arr_start = content.find('[');
        if (arr_start == std::string::npos)
            return result;

        size_t depth = 0;
        size_t obj_start = std::string::npos;
        for (size_t i = arr_start; i < content.size(); ++i)
        {
            char c = content[i];
            if (c == '{')
            {
                if (depth == 0)
                    obj_start = i;
                ++depth;
            }
            else if (c == '}')
            {
                if (depth == 0)
                    continue;
                --depth;
                if (depth == 0 && obj_start != std::string::npos)
                {
                    std::string obj = content.substr(obj_start, i - obj_start + 1);

                    auto get_str = [&](const std::string &key) -> std::string
                    {
                        std::string needle = "\"" + key + "\"";
                        size_t p = obj.find(needle);
                        if (p == std::string::npos)
                            return "";
                        p = obj.find('"', p + needle.size());
                        if (p == std::string::npos)
                            return "";
                        size_t end = obj.find('"', p + 1);
                        if (end == std::string::npos)
                            return "";
                        return obj.substr(p + 1, end - p - 1);
                    };

                    RepoPackage pkg;
                    pkg.name = get_str("name");
                    if (pkg.name.empty())
                    {
                        obj_start = std::string::npos;
                        continue;
                    }
                    pkg.version = get_str("version");
                    pkg.arch = get_str("arch");
                    pkg.repo = get_str("repo");
                    pkg.size = get_str("size");
                    pkg.description = get_str("description");
                    pkg.sha256 = get_str("sha256");
                    pkg.url = get_str("url");

                    if (pkg.url.empty() && !base.empty())
                        pkg.url = base + "/" + pkg.name + "-" + pkg.version +
                                  "." + pkg.arch + ".fpm";

                    if (!pkg.name.empty() && !pkg.version.empty() && !pkg.arch.empty())
                        result.push_back(std::move(pkg));
                    obj_start = std::string::npos;
                }
            }
        }
        return result;
    }

    static bool fpm_index_valid(const std::vector<RepoPackage> &pkgs)
    {
        if (pkgs.empty())
            return false;
        size_t with_hash = 0;
        for (const auto &p : pkgs)
        {
            if (p.name.empty() || p.version.empty() || p.arch.empty())
                return false;
            if (looks_like_sha256(p.sha256))
                ++with_hash;
        }
        return with_hash >= 1;
    }

    class FpmAdapter : public RepoAdapter
    {
    public:
        std::string format_name() const override { return "FPM"; }

        AdapterOutcome probe(const std::string &base,
                             const FileFetcher &fetch,
                             size_t max_packages) const override
        {
            AdapterOutcome out;
            std::string root = base;
            while (!root.empty() && root.back() == '/')
                root.pop_back();

            (void)max_packages; // FPM indexes are small; parse them fully.

            for (const auto &cand : fpm_index_candidates())
            {
                std::string url = root + cand;
                std::string tmp = DatabaseManager::get_repo_cache_path() + "/probe_fpm.tmp";
                std::error_code ec;
                fs::remove(tmp, ec);

                if (!fetch(url, tmp, 5000))
                {
                    int code = MirrorSelector::http_status(url, 3000);
                    std::string reason;
                    if (code < 0)
                        reason = "connection failed";
                    else if (code == 404)
                        reason = "not found (HTTP 404)";
                    else
                        reason = "HTTP " + std::to_string(code);
                    out.note = reason;
                    out.note_prio = 10; // HTTP-level diagnostics take precedence
                    continue;
                }

                std::ifstream in(tmp, std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                in.close();
                std::string content = ss.str();
                fs::remove(tmp, ec);

                if (content.find('\0') != std::string::npos || content.size() < 4)
                {
                    out.note = "binary/invalid content";
                    out.note_prio = 10;
                    continue;
                }

                bool is_json = (!content.empty() && (content[0] == '{' || content[0] == '['));
                auto pkgs = is_json ? parse_fpm_json(content, root)
                                    : parse_fpm_text(content, root);

                if (fpm_index_valid(pkgs))
                {
                    out.ok = true;
                    out.packages = std::move(pkgs);
                    out.source = url;
                    out.note = "";
                    return out;
                }
                out.note = "invalid index content (no valid package records)";
                out.note_prio = 10;
                break; // A real index was fetched but invalid; stop probing.
            }
            return out;
        }
    };

    std::unique_ptr<RepoAdapter> make_fpm_adapter()
    {
        return std::make_unique<FpmAdapter>();
    }

    // -----------------------------------------------------------------------
    // Arch Linux adapter
    // -----------------------------------------------------------------------

    static const std::vector<std::string> arch_repos = {"core", "extra", "multilib"};
    static const std::vector<std::string> arch_archs = {"x86_64", "aarch64"};

    static std::string arch_map_repo(const std::string &r)
    {
        if (r == "core") return "core";
        if (r == "extra") return "extra";
        if (r == "multilib") return "multilib";
        return r;
    }

    // Parses a single Arch package "desc" metadata file.  The format is:
    //     %KEY%
    //     value text (possibly multiple lines for depends)
    //     %KEY2%
    //     value
    // Records are separated by the files being separate per package; the
    // desc file holds FILENAME, NAME, VERSION, DESC, CSIZE/ISIZE, SHA256SUM,
    // ARCH and DEPENDS (one per line).
    static RepoPackage parse_arch_desc_file(const std::string &path)
    {
        RepoPackage pkg;
        std::ifstream in(path);
        if (!in.is_open())
            return pkg;

        std::string line;
        std::string current_key;
        std::string current_value;

        auto flush = [&]()
        {
            if (current_key.empty())
                return;

            if (current_key == "%FILENAME%")
                pkg.url = trim(current_value);
            else if (current_key == "%NAME%")
                pkg.name = trim(current_value);
            else if (current_key == "%VERSION%")
                pkg.version = trim(current_value);
            else if (current_key == "%ARCH%")
                pkg.arch = trim(current_value);
            else if (current_key == "%DESC%")
                pkg.description = trim(current_value);
            else if (current_key == "%CSIZE%" || current_key == "%ISIZE%")
            {
                std::string v = trim(current_value);
                try
                {
                    uint64_t bytes = std::stoull(v);
                    if (bytes < 1024)
                        pkg.size = v + " B";
                    else if (bytes < 1024 * 1024)
                        pkg.size = std::to_string(bytes / 1024) + " KiB";
                    else
                        pkg.size = std::to_string(bytes / (1024 * 1024)) + " MiB";
                }
                catch (...)
                {
                    if (pkg.size.empty())
                        pkg.size = v;
                }
            }
            else if (current_key == "%SHA256SUM%")
                pkg.sha256 = trim(current_value);
            else if (current_key == "%DEPENDS%")
            {
                std::istringstream ds(current_value);
                std::string dep;
                while (std::getline(ds, dep))
                {
                    std::string dn = normalize_dep_name(dep);
                    if (!dn.empty())
                        pkg.depends.push_back(dn);
                }
            }

            current_key.clear();
            current_value.clear();
        };

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty())
            {
                flush();
                continue;
            }

            if (line[0] == '%')
            {
                size_t end = line.rfind('%');
                if (end > 0)
                {
                    flush();
                    current_key = line.substr(0, end + 1);
                }
                continue;
            }

            if (!current_key.empty())
            {
                if (!current_value.empty())
                    current_value += "\n";
                current_value += line;
            }
        }
        flush();

        return pkg;
    }

    // Reads extracted Arch metadata files and builds RepoPackage objects.
    // Each package directory contains a "desc" file with the full record.
    // Returns a vector of fully-populated package entries with download URLs.
    static std::vector<RepoPackage> parse_arch_metadata_dir(
        const std::string &dir, const std::string &repo_base_url)
    {
        std::vector<RepoPackage> result;

        if (!fs::exists(dir))
            return result;

        for (const auto &entry : fs::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().filename() != "desc")
                continue;

            RepoPackage pkg = parse_arch_desc_file(entry.path().string());
            if (pkg.name.empty())
                continue;

            if (!pkg.url.empty() && !repo_base_url.empty())
            {
                std::string full_url = repo_base_url;
                if (full_url.back() != '/')
                    full_url += '/';
                full_url += pkg.url;
                pkg.url = full_url;
            }

            result.push_back(std::move(pkg));
        }

        return result;
    }

    // Downloads an Arch .db file, extracts the metadata, and returns parsed
    // package records.  The .db format is a gzipped tar archive containing
    // one metadata file per package (with %KEY% = value lines separated by
    // blank lines).
    static bool extract_arch_repo(const std::string &db_url,
                                  const std::string &db_name,
                                  const std::string &repo_base_url,
                                  std::vector<RepoPackage> &out)
    {
        std::string tmp_dir = DatabaseManager::get_repo_cache_path() + "/arch_tmp_" + db_name;
        std::string db_file = tmp_dir + "/repo.db";
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
        fs::create_directories(tmp_dir, ec);

        std::string dl_cmd = "curl -sLf --connect-timeout 8 --max-time 180"
                             " --speed-limit 32 --speed-time 60"
                             " -o " + db_file + " " + db_url + " 2>/dev/null";

        int status = system(dl_cmd.c_str());
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            fs::remove_all(tmp_dir, ec);
            return false;
        }

        uintmax_t sz = fs::file_size(db_file, ec);
        if (ec || sz < 64)
        {
            fs::remove_all(tmp_dir, ec);
            return false;
        }

        std::string meta_dir = tmp_dir + "/meta";
        fs::create_directories(meta_dir, ec);

        // Arch .db is a gzipped tar archive containing per-package metadata
        // directories, each with a "desc" file.  Extract only those.
        std::string tar_cmd = "tar xf " + db_file + " --auto-compress -C " +
                              meta_dir + " --wildcards --no-anchored '*/desc' 2>/dev/null";

        status = system(tar_cmd.c_str());
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            tar_cmd = "tar xzf " + db_file + " -C " +
                      meta_dir + " --wildcards --no-anchored '*/desc' 2>/dev/null";
            status = system(tar_cmd.c_str());
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            // Try extracting all files (some .db archives have flat layout)
            tar_cmd = "tar xf " + db_file + " --auto-compress -C " + meta_dir + " 2>/dev/null";
            status = system(tar_cmd.c_str());
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                tar_cmd = "tar xzf " + db_file + " -C " + meta_dir + " 2>/dev/null";
                system(tar_cmd.c_str());
            }
        }

        auto pkgs = parse_arch_metadata_dir(meta_dir, repo_base_url);
        out.insert(out.end(), pkgs.begin(), pkgs.end());

        fs::remove_all(tmp_dir, ec);
        return !pkgs.empty();
    }

    class ArchAdapter : public RepoAdapter
    {
    public:
        std::string format_name() const override { return "Arch"; }

        // Probes the mirror for Arch Linux repository structure.  Checks
        // standard Arch repo paths (core, extra, multilib) for x86_64 and
        // aarch64, downloads .db files, extracts package metadata, and
        // returns unified RepoPackage records.
        AdapterOutcome probe(const std::string &base,
                             const FileFetcher &fetch,
                             size_t max_packages) const override
        {
            AdapterOutcome out;
            std::string root = base;
            while (!root.empty() && root.back() == '/')
                root.pop_back();

            (void)max_packages; // Arch .db contains the full repo already.

            for (const auto &arch : arch_archs)
            {
                for (const auto &repo : arch_repos)
                {
                    std::string db_name = repo + ".db";
                    std::string db_url = root + "/" + repo + "/os/" + arch + "/" + db_name;
                    std::string repo_url = root + "/" + repo + "/os/" + arch;

                    int code = MirrorSelector::http_status(db_url, 4000);
                    if (code != 200)
                        continue;

                    std::vector<RepoPackage> pkgs;
                    if (extract_arch_repo(db_url, db_name + "_" + arch, repo_url, pkgs))
                    {
                        for (auto &p : pkgs)
                        {
                            if (p.repo.empty())
                                p.repo = arch_map_repo(repo);
                            if (p.arch.empty())
                                p.arch = arch;
                        }

                        if (!out.ok || pkgs.size() > out.packages.size())
                        {
                            out.ok = true;
                            out.packages.insert(out.packages.end(),
                                                std::make_move_iterator(pkgs.begin()),
                                                std::make_move_iterator(pkgs.end()));
                            out.source = db_url;
                            out.note = "";
                        }
                    }
                }
            }
            if (!out.ok && out.note.empty())
                out.note = "no Arch repositories found (core/extra/multilib)";
            return out;
        }
    };

    std::unique_ptr<RepoAdapter> make_arch_adapter()
    {
        return std::make_unique<ArchAdapter>();
    }

    // -----------------------------------------------------------------------
    // Debian/Ubuntu adapter
    // -----------------------------------------------------------------------

    static RepoPackage parse_debian_package_entry(const std::string &entry)
    {
        RepoPackage pkg;
        std::istringstream in(entry);
        std::string line;

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty() || (!std::isspace(static_cast<unsigned char>(line[0])) && line[0] != '#'))
            {
                // Continuation lines start with whitespace; skip them.
            }

            auto eq = line.find(':');
            if (eq == std::string::npos)
                continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "Package")
                pkg.name = val;
            else if (key == "Version")
                pkg.version = val;
            else if (key == "Architecture")
                pkg.arch = val;
            else if (key == "Filename")
                pkg.url = val;
            else if (key == "Size")
                pkg.size = val;
            else if (key == "Description")
                pkg.description = val;
            else if (key == "Depends" || key == "Pre-Depends")
            {
                // Debian deps are comma-separated groups (alternatives with |).
                // We take the first package name from each group.
                std::istringstream ds(val);
                std::string group;
                while (std::getline(ds, group, ','))
                {
                    std::string dep = trim(group);
                    size_t pipe = dep.find('|');
                    if (pipe != std::string::npos)
                        dep = dep.substr(0, pipe);
                    // Strip version constraint
                    size_t paren = dep.find('(');
                    if (paren != std::string::npos)
                        dep = dep.substr(0, paren);
                    dep = trim(dep);
                    if (!dep.empty())
                        pkg.depends.push_back(dep);
                }
            }
        }

        return pkg;
    }

    // Parses a Debian Release file and extracts suite, components, and
    // architectures.  Returns vectors of components (e.g., "main") and
    // architectures (e.g., "amd64") found in the file.
    static void parse_debian_release(const std::string &content,
                                     std::string &suite,
                                     std::vector<std::string> &components,
                                     std::vector<std::string> &architectures)
    {
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto eq = line.find(':');
            if (eq == std::string::npos)
                continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "Suite")
                suite = val;
            else if (key == "Components")
            {
                std::istringstream ss(val);
                std::string c;
                while (std::getline(ss, c, ' '))
                {
                    c = trim(c);
                    if (!c.empty())
                        components.push_back(c);
                }
            }
            else if (key == "Architectures")
            {
                std::istringstream ss(val);
                std::string a;
                while (std::getline(ss, a, ' '))
                {
                    a = trim(a);
                    if (!a.empty())
                        architectures.push_back(a);
                }
            }
        }
    }

    // Decompress a downloaded index file (".gz"/".xz"/".bz2"/".zst"/""plain)
    // into <dst>. Returns true on success and a non-empty output at <dst>.
    static bool decompress_to(const std::string &src, const std::string &dst,
                              const char *ext)
    {
        std::string cmd;
        if (ext == nullptr || *ext == '\0')
        {
            std::error_code ec;
            return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) && fs::file_size(dst) > 0;
        }
        else if (std::string(ext) == ".gz")
        {
            cmd = "gzip -dc '" + src + "' > '" + dst + "'";
        }
        else if (std::string(ext) == ".xz")
        {
            cmd = "xz -dc '" + src + "' > '" + dst + "'";
        }
        else if (std::string(ext) == ".bz2")
        {
            cmd = "bzip2 -dc '" + src + "' > '" + dst + "'";
        }
        else if (std::string(ext) == ".zst")
        {
            cmd = "zstd -dc '" + src + "' -o '" + dst + "'";
        }
        else
        {
            return false;
        }

        int rc = std::system((cmd + " 2>/dev/null").c_str());
        std::error_code ec;
        return rc == 0 && fs::file_size(dst, ec) > 0;
    }

    class DebianAdapter : public RepoAdapter
    {
    public:
        std::string format_name() const override { return "Debian"; }

        // Map the running machine architecture to a Debian arch id.
        static std::string native_debian_arch()
        {
#ifdef __x86_64__
            return "amd64";
#elif defined(__i386__)
            return "i386";
#elif defined(__aarch64__)
            return "arm64";
#elif defined(__arm__)
            return "armhf";
#elif defined(__riscv)
            return "riscv64";
#elif defined(__s390x__)
            return "s390x";
#elif defined(__powerpc64__)
            return "ppc64el";
#else
            return "amd64";
#endif
        }

        AdapterOutcome probe(const std::string &base,
                             const FileFetcher &fetch,
                             size_t max_packages) const override
        {
            AdapterOutcome out;
            std::string root = base;
            while (!root.empty() && root.back() == '/')
                root.pop_back();

            // Native Arch probe device uses native arch; probe/update mode:
            // when max_packages is non-zero we stop early (mirror detection);
            // a full update parses the whole index.
            (void)max_packages;

            // Preferred suites, native architecture first.
            static const std::string suites[] = {"bookworm", "bullseye", "trixie",
                                                 "noble", "jammy", "focal", "buster"};
            const std::string native = native_debian_arch();
            std::vector<std::string> archs = {native};
            if (native != "amd64")
                archs.push_back("amd64");
            if (native != "arm64")
                archs.push_back("arm64");

            // Prefer the smallest compressed index; plain Packages is usually
            // absent (404) on published mirrors.
            static const char *exts[] = {".xz", ".gz", ".bz2", ".zst", ""};

            static const std::string dists_prefix = "/dists/";

            for (const auto &suite : suites)
            {
                std::string release_url = root + dists_prefix + suite + "/Release";
                std::string tmp = DatabaseManager::get_repo_cache_path() + "/probe_debian.tmp";
                std::error_code ec;
                fs::remove(tmp, ec);

                if (!fetch(release_url, tmp, 5000))
                    continue;

                std::ifstream in(tmp);
                std::ostringstream ss;
                ss << in.rdbuf();
                in.close();
                std::string content = ss.str();
                fs::remove(tmp, ec);

                if (content.find("Codename") == std::string::npos &&
                    content.find("Suite") == std::string::npos &&
                    content.find("Components") == std::string::npos)
                {
                    continue;
                }

                std::string parsed_suite;
                std::vector<std::string> components, architectures;
                parse_debian_release(content, parsed_suite, components, architectures);

                if (components.empty())
                    components = {"main"};
                if (architectures.empty())
                    architectures = archs;

                for (const auto &arch : archs)
                {
                    if (std::find(architectures.begin(), architectures.end(), arch) ==
                        architectures.end())
                        continue;

                    for (const auto &comp : components)
                    {
                        std::string pkg_url = root + dists_prefix + suite + "/" +
                                              comp + "/binary-" + arch + "/Packages";

                        std::string ptmp = DatabaseManager::get_repo_cache_path() +
                                           "/probe_debian_pkgs.tmp";
                        fs::remove(ptmp, ec);

                        std::string fetched;
                        bool download_ok = false;
                        for (const auto *ext : exts)
                        {
                            std::string cand = pkg_url + ext;
                            std::string tmp2 = ptmp + ".dl";
                            fs::remove(tmp2, ec);
                            if (!fetch(cand, tmp2, 8000))
                                continue;
                            std::string fn = ptmp + ".out";
                            if (!decompress_to(tmp2, fn, ext))
                            {
                                fs::remove(tmp2, ec);
                                continue;
                            }
                            fs::remove(tmp2, ec);
                            fetched = cand;
                            fs::rename(fn, ptmp, ec);
                            download_ok = true;
                            break;
                        }
                        if (!download_ok)
                            continue;

                        std::ifstream pin(ptmp);
                        std::ostringstream pss;
                        pss << pin.rdbuf();
                        pin.close();
                        std::string pkg_content = pss.str();
                        fs::remove(ptmp, ec);

                        if (pkg_content.find("Package:") == std::string::npos)
                            continue;

                        // Parse package entries (separated by blank lines).
                        std::istringstream pin_stream(pkg_content);
                        std::string pkg_line;
                        std::string current_entry;
                        size_t count = 0;

                        while (std::getline(pin_stream, pkg_line))
                        {
                            if (!pkg_line.empty() && pkg_line.back() == '\r')
                                pkg_line.pop_back();

                            if (pkg_line.empty())
                            {
                                if (!current_entry.empty())
                                {
                                    auto pkg = parse_debian_package_entry(current_entry);
                                    if (!pkg.name.empty() && !pkg.url.empty())
                                    {
                                        pkg.repo = comp;
                                        if (pkg.arch.empty())
                                            pkg.arch = arch;

                                        // Prepend root to relative Filename
                                        if (!pkg.url.empty() && pkg.url[0] != '/')
                                            pkg.url = root + "/" + pkg.url;

                                        out.packages.push_back(std::move(pkg));
                                        ++count;
                                    }
                                    current_entry.clear();
                                }
                                // Probe/update mode limit.
                                if (max_packages > 0 && count >= max_packages)
                                    break;
                            }
                            else
                            {
                                current_entry += pkg_line + "\n";
                            }
                        }
                        // Process last entry if file doesn't end with blank line.
                        if (!current_entry.empty())
                        {
                            auto pkg = parse_debian_package_entry(current_entry);
                            if (!pkg.name.empty() && !pkg.url.empty())
                            {
                                pkg.repo = comp;
                                if (pkg.arch.empty())
                                    pkg.arch = arch;
                                if (!pkg.url.empty() && pkg.url[0] != '/')
                                    pkg.url = root + "/" + pkg.url;
                                out.packages.push_back(std::move(pkg));
                                ++count;
                            }
                        }

                        if (count > 0 && !out.ok)
                        {
                            out.ok = true;
                            out.source = fetched;
                            return out;
                        }
                    }
                }
            }

            out.note = "no Debian repositories found (no valid Release/Packages)";
            return out;
        }
    };

    std::unique_ptr<RepoAdapter> make_debian_adapter()
    {
        return std::make_unique<DebianAdapter>();
    }

    // -----------------------------------------------------------------------
    // Alpine adapter
    // -----------------------------------------------------------------------

    static const std::vector<std::string> alpine_repos = {"main", "community", "testing"};

    static RepoPackage parse_apk_index_entry(const std::string &entry)
    {
        RepoPackage pkg;
        std::istringstream in(entry);
        std::string line;

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            auto eq = line.find(':');
            if (eq == std::string::npos)
                continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "P")
                pkg.name = val;
            else if (key == "V")
                pkg.version = val;
            else if (key == "A")
                pkg.arch = val;
            else if (key == "T")
                pkg.description = val;
            else if (key == "U")
                pkg.url = val;
            else if (key == "D")
            {
                // Alpine deps: "dep1>=1.0 dep2>=2.0" space-separated.
                std::istringstream ds(val);
                std::string dep;
                while (std::getline(ds, dep, ' '))
                {
                    std::string dn = normalize_dep_name(dep);
                    if (!dn.empty())
                        pkg.depends.push_back(dn);
                }
            }
            else if (key == "C")
                pkg.sha256 = val;
            else if (key == "I")
            {
                try
                {
                    uint64_t bytes = std::stoull(val);
                    if (bytes < 1024)
                        pkg.size = val + " B";
                    else if (bytes < 1024 * 1024)
                        pkg.size = std::to_string(bytes / 1024) + " KiB";
                    else
                        pkg.size = std::to_string(bytes / (1024 * 1024)) + " MiB";
                }
                catch (...)
                {
                    pkg.size = val;
                }
            }
        }

        return pkg;
    }

    class AlpineAdapter : public RepoAdapter
    {
    public:
        std::string format_name() const override { return "Alpine"; }

        AdapterOutcome probe(const std::string &base,
                             const FileFetcher &fetch,
                             size_t max_packages) const override
        {
            AdapterOutcome out;
            std::string root = base;
            while (!root.empty() && root.back() == '/')
                root.pop_back();

            // Alpine v3.x is a safe default for probing.
            static const std::string versions[] = {"v3.19", "v3.20", "edge"};

            for (const auto &ver : versions)
            {
                for (const auto &repo : alpine_repos)
                {
                    // Alpine APK index: plain text file, tab-separated fields.
                    std::string index_url = root + "/" + ver + "/" + repo + "/" +
                                            "x86_64/APKINDEX.tar.gz";
                    std::string tmp = DatabaseManager::get_repo_cache_path() +
                                      "/probe_alpine.tmp";
                    std::error_code ec;
                    fs::remove(tmp, ec);

                    if (!fetch(index_url, tmp, 5000))
                        continue;

                    // The APK index is a gzipped tar containing a single
                    // "APKINDEX" plain-text file.  Extract it.
                    std::string tmp_dir = DatabaseManager::get_repo_cache_path() +
                                          "/alpine_tmp_" + ver + "_" + repo;
                    fs::remove_all(tmp_dir, ec);
                    fs::create_directories(tmp_dir, ec);

                    std::string extract_cmd = "tar xzf " + tmp + " -C " + tmp_dir + " 2>/dev/null";
                    system(extract_cmd.c_str());

                    std::string apkindex_path = tmp_dir + "/APKINDEX";
                    if (!fs::exists(apkindex_path))
                    {
                        // Try finding it in subdirectories.
                        for (const auto &e : fs::recursive_directory_iterator(tmp_dir, ec))
                        {
                            if (e.path().filename() == "APKINDEX")
                            {
                                apkindex_path = e.path().string();
                                break;
                            }
                        }
                    }

                    if (!fs::exists(apkindex_path))
                    {
                        fs::remove_all(tmp_dir, ec);
                        fs::remove(tmp, ec);
                        continue;
                    }

                    std::ifstream pin(apkindex_path);
                    std::string pin_content((std::istreambuf_iterator<char>(pin)),
                                            std::istreambuf_iterator<char>());
                    pin.close();
                    fs::remove_all(tmp_dir, ec);
                    fs::remove(tmp, ec);

                    // Parse entries separated by blank lines.
                    std::istringstream pin_stream(pin_content);
                    std::string line;
                    std::string current_entry;
                    size_t count = 0;

                    while (std::getline(pin_stream, line))
                    {
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();

                        if (line.empty())
                        {
                            if (!current_entry.empty())
                            {
                                auto pkg = parse_apk_index_entry(current_entry);
                                if (!pkg.name.empty() && !pkg.version.empty())
                                {
                                    pkg.repo = repo;
                                    if (pkg.arch.empty())
                                        pkg.arch = "x86_64";

                                    // Construct download URL.
                                    // Alpine: <base>/<ver>/<repo>/<arch>/<pkg>-<ver>.apk
                                    // (the APKINDEX "U:" field is the upstream
                                    // homepage, never the mirror .apk path)
                                    pkg.url = root + "/" + ver + "/" + repo +
                                              "/" + pkg.arch + "/" + pkg.name +
                                              "-" + pkg.version + ".apk";

                                    out.packages.push_back(std::move(pkg));
                                    ++count;
                                }
                                current_entry.clear();
                            }
                            if (max_packages > 0 && count >= max_packages)
                                break;
                        }
                        else
                        {
                            current_entry += line + "\n";
                        }
                    }
                    // Process last entry.
                    if (!current_entry.empty())
                    {
                        auto pkg = parse_apk_index_entry(current_entry);
                        if (!pkg.name.empty() && !pkg.version.empty())
                        {
                            pkg.repo = repo;
                            if (pkg.arch.empty())
                                pkg.arch = "x86_64";
                            pkg.url = root + "/" + ver + "/" + repo +
                                      "/" + pkg.arch + "/" + pkg.name +
                                      "-" + pkg.version + ".apk";
                            out.packages.push_back(std::move(pkg));
                            ++count;
                        }
                    }

                    if (count > 0 && !out.ok)
                    {
                        out.ok = true;
                        out.source = index_url;
                    }
                    if (max_packages > 0 && out.packages.size() >= max_packages)
                        return out;
                }
                if (out.ok)
                    return out;
            }

            out.note = "no Alpine repositories found";
            return out;
        }
    };

    std::unique_ptr<RepoAdapter> make_alpine_adapter()
    {
        return std::make_unique<AlpineAdapter>();
    }

    // -----------------------------------------------------------------------
    // RPM (Fedora/CentOS) adapter
    // -----------------------------------------------------------------------

    static const std::vector<std::string> rpm_repos_fedora = {
        "linux/releases/44/Everything/x86_64/os",
        "linux/releases/43/Everything/x86_64/os",
        "linux/releases/42/Everything/x86_64/os",
        "linux/updates/44/x86_64",
        "linux/updates/43/x86_64",
        "linux/updates/42/x86_64",
        "linux/rawhide/Everything/x86_64/os",
        "releases/41/Everything/x86_64/os",
        "releases/40/Everything/x86_64/os",
    };

    static const std::vector<std::string> rpm_repos_centos = {
        "9-stream/BaseOS/x86_64/os",
        "8-stream/BaseOS/x86_64/os",
    };

    // Parses an RPM primary.xml entry and populates a RepoPackage.
    // The XML is in the rpm primary.xml namespace: each package has
    // <name>, <version>, <arch>, <summary>, <size>, <location>, and
    // <requires><entry name="..."/></requires> children.
    static RepoPackage parse_rpm_primary_entry(const std::string &entry)
    {
        RepoPackage pkg;

        auto get_tag = [&](const std::string &tag) -> std::string
        {
            // Handles both <tag>value</tag> and <tag attr="val" />
            std::string open = "<" + tag;
            size_t p = entry.find(open);
            if (p == std::string::npos)
                return "";

            // Find the closing '>' of the opening tag.
            size_t close = entry.find('>', p);
            if (close == std::string::npos)
                return "";

            // Self-closing tag: <tag ... />
            if (entry[close - 1] == '/')
                return "";

            size_t val_start = close + 1;
            size_t val_end = entry.find("</" + tag + ">", val_start);
            if (val_end == std::string::npos)
                return "";

            return trim(entry.substr(val_start, val_end - val_start));
        };

        auto get_attr = [&](const std::string &tag,
                            const std::string &attr) -> std::string
        {
            std::string needle = "<" + tag;
            size_t p = entry.find(needle);
            if (p == std::string::npos)
                return "";

            size_t attr_p = entry.find(attr + "=\"", p);
            if (attr_p == std::string::npos)
                return "";

            size_t val_start = attr_p + attr.size() + 2;
            size_t val_end = entry.find('"', val_start);
            if (val_end == std::string::npos)
                return "";

            return entry.substr(val_start, val_end - val_start);
        };

        pkg.name = get_tag("name");
        if (pkg.name.empty())
            return pkg;

        pkg.version = get_attr("version", "ver");
        if (pkg.version.empty())
            pkg.version = get_tag("version");
        pkg.arch = get_tag("arch");
        pkg.description = get_tag("summary");

        // Size
        std::string size_str = get_attr("size", "installed");
        if (!size_str.empty())
        {
            try
            {
                uint64_t bytes = std::stoull(size_str);
                if (bytes < 1024)
                    pkg.size = size_str + " B";
                else if (bytes < 1024 * 1024)
                    pkg.size = std::to_string(bytes / 1024) + " KiB";
                else
                    pkg.size = std::to_string(bytes / (1024 * 1024)) + " MiB";
            }
            catch (...)
            {
                pkg.size = size_str;
            }
        }

        // Download URL: <location href="..." />
        std::string loc = get_attr("location", "href");
        if (!loc.empty())
            pkg.url = loc;

        // Dependencies: <requires><entry name="..." /></requires>
        size_t req_start = entry.find("<requires>");
        size_t req_end = entry.find("</requires>", req_start);
        if (req_start != std::string::npos && req_end != std::string::npos)
        {
            std::string req_block = entry.substr(req_start, req_end - req_start);
            size_t pos = 0;
            while ((pos = req_block.find("<entry", pos)) != std::string::npos)
            {
                size_t name_p = req_block.find("name=\"", pos);
                if (name_p == std::string::npos || name_p > req_end)
                    break;
                size_t val_start = name_p + 6;
                size_t val_end = req_block.find('"', val_start);
                if (val_end == std::string::npos)
                    break;
                std::string dep_name = req_block.substr(val_start, val_end - val_start);
                dep_name = normalize_dep_name(dep_name);
                if (!dep_name.empty())
                    pkg.depends.push_back(dep_name);
                pos = val_end + 1;
            }
        }

        return pkg;
    }

    class RpmAdapter : public RepoAdapter
    {
    public:
        std::string format_name() const override { return "RPM"; }

        AdapterOutcome probe(const std::string &base,
                             const FileFetcher &fetch,
                             size_t max_packages) const override
        {
            AdapterOutcome out;
            std::string root = base;
            while (!root.empty() && root.back() == '/')
                root.pop_back();

            std::vector<std::string> all_paths;
            all_paths.insert(all_paths.end(), rpm_repos_fedora.begin(), rpm_repos_fedora.end());
            all_paths.insert(all_paths.end(), rpm_repos_centos.begin(), rpm_repos_centos.end());

            for (const auto &rpath : all_paths)
            {
                std::string repomd_url = root + "/" + rpath + "/repodata/repomd.xml";
                std::string tmp = DatabaseManager::get_repo_cache_path() +
                                  "/probe_rpm.tmp";
                std::error_code ec;
                fs::remove(tmp, ec);

                if (!fetch(repomd_url, tmp, 8000))
                    continue;

                std::ifstream in(tmp);
                std::ostringstream ss;
                ss << in.rdbuf();
                in.close();
                std::string repomd_content = ss.str();
                fs::remove(tmp, ec);

                // Locate the primary metadata location + compression in repomd.xml:
                //   <data type="primary"><location href="repodata/…-primary.xml.gz"/></data>
                std::string primary_href;
                std::string primary_ext;
                {
                    size_t dp = repomd_content.find("<data type=\"primary\"");
                    if (dp == std::string::npos)
                        dp = repomd_content.find("type=\"primary\"");
                    if (dp == std::string::npos)
                        continue;
                    size_t loc = repomd_content.find("<location", dp);
                    size_t href_p = repomd_content.find("href=\"", loc);
                    if (loc == std::string::npos || href_p == std::string::npos)
                        continue;
                    href_p += 6;
                    size_t href_end = repomd_content.find('"', href_p);
                    if (href_end == std::string::npos)
                        continue;
                    primary_href = repomd_content.substr(href_p, href_end - href_p);
                }

                if (primary_href.empty())
                    continue;

                // Determine compression from the href extension.
                if (primary_href.size() > 3 &&
                    primary_href.compare(primary_href.size() - 3, 3, ".gz") == 0)
                    primary_ext = ".gz";
                else if (primary_href.size() > 3 &&
                         primary_href.compare(primary_href.size() - 3, 3, ".xz") == 0)
                    primary_ext = ".xz";
                else if (primary_href.size() > 4 &&
                         primary_href.compare(primary_href.size() - 4, 4, ".zst") == 0)
                    primary_ext = ".zst";
                else if (primary_href.size() > 4 &&
                         primary_href.compare(primary_href.size() - 4, 4, ".bz2") == 0)
                    primary_ext = ".bz2";

                std::string primary_url = primary_href;
                if (!primary_url.empty() && primary_url[0] == '/')
                    primary_url = root + primary_url;
                else if (!primary_url.empty() &&
                         primary_url.rfind("http://", 0) != 0 &&
                         primary_url.rfind("https://", 0) != 0)
                {
                    // repomd.xml <location href> paths are relative to the
                    // repository base (e.g. "repodata/…-primary.xml.zst").
                    primary_url = root + "/" + rpath + "/" + primary_url;
                }

                std::string pdl = DatabaseManager::get_repo_cache_path() +
                                  "/probe_rpm_primary.tmp";
                fs::remove(pdl, ec);
                if (!fetch(primary_url, pdl, 8000))
                    continue;

                std::string plain = DatabaseManager::get_repo_cache_path() +
                                    "/probe_rpm_primary_plain.tmp";
                fs::remove(plain, ec);
                if (!decompress_to(pdl, plain, primary_ext.empty() ? nullptr : primary_ext.c_str()))
                {
                    fs::remove(pdl, ec);
                    continue;
                }
                fs::remove(pdl, ec);

                std::ifstream pin(plain);
                std::ostringstream pss;
                pss << pin.rdbuf();
                pin.close();
                std::string content = pss.str();
                fs::remove(plain, ec);

                if (content.find("<package") == std::string::npos)
                    continue;

                // Parse each <package ...>...</package> block.
                size_t pos = 0;
                size_t count = 0;
                while ((pos = content.find("<package", pos)) != std::string::npos)
                {
                    size_t open_end = content.find('>', pos);
                    if (open_end == std::string::npos)
                        break;
                    size_t end = content.find("</package>", open_end);
                    if (end == std::string::npos)
                        break;
                    std::string entry = content.substr(pos, end - pos + 10);
                    pos = end + 10;

                    auto pkg = parse_rpm_primary_entry(entry);
                    if (!pkg.name.empty() && !pkg.version.empty())
                    {
                        if (pkg.arch.empty())
                            pkg.arch = "x86_64";

                        // <location href> is relative to the repository base
                        // directory (rpath), e.g. "Packages/0/0ad-….rpm".
                        if (!pkg.url.empty() &&
                            pkg.url.rfind("http://", 0) != 0 &&
                            pkg.url.rfind("https://", 0) != 0)
                        {
                            if (pkg.url[0] == '/')
                                pkg.url = root + pkg.url;
                            else
                                pkg.url = root + "/" + rpath + "/" + pkg.url;
                        }

out.packages.push_back(std::move(pkg));
                        ++count;
                        if (max_packages > 0 && count >= max_packages)
                            break;
                    }
                }

                if (count > 0 && !out.ok)
                {
                    out.ok = true;
                    out.source = primary_url;
                    return out;
                }
            }

            out.note = "no RPM repositories found (Fedora/CentOS)";
            return out;
        }
    };

    std::unique_ptr<RepoAdapter> make_rpm_adapter()
    {
        return std::make_unique<RpmAdapter>();
    }

    // -----------------------------------------------------------------------
    // Adapter registry and probe orchestration
    // -----------------------------------------------------------------------

    std::vector<std::unique_ptr<RepoAdapter>> default_adapters()
    {
        std::vector<std::unique_ptr<RepoAdapter>> v;
        v.push_back(make_fpm_adapter());
        v.push_back(make_arch_adapter());
        v.push_back(make_debian_adapter());
        v.push_back(make_alpine_adapter());
        v.push_back(make_rpm_adapter());
        return v;
    }

    ProbeResult probe_mirror(const std::string &base, std::string &fail_reason,
                             int timeout_ms, size_t max_packages)
    {
        (void)timeout_ms;

        ProbeResult result;
        std::string best_fail;
        int best_prio = -1;

        auto adapters = default_adapters();

        auto file_fetcher = [](const std::string &url, const std::string &path,
                               int timeout) -> bool
        {
            int tsec = timeout / 1000;
            if (tsec < 1)
                tsec = 1;
            return MirrorSelector::download_fast(url, path);
        };

        for (auto &adapter : adapters)
        {
            auto outcome = adapter->probe(base, file_fetcher, max_packages);

            if (outcome.ok)
            {
                // Filter out packages with missing required fields.
                std::vector<RepoPackage> valid;
                for (auto &p : outcome.packages)
                {
                    if (!p.name.empty() && !p.version.empty() && !p.arch.empty())
                        valid.push_back(std::move(p));
                }

                if (!valid.empty())
                {
                    result.ok = true;
                    result.format = adapter->format_name();
                    result.source = outcome.source;
                    result.packages = std::move(valid);
                    fail_reason.clear();
                    return result;
                }
            }
            else if (!outcome.note.empty())
            {
                int prio = outcome.note_prio;
                if (prio > best_prio || (prio == best_prio && outcome.note.size() > best_fail.size()))
                {
                    best_fail = outcome.note;
                    best_prio = prio;
                }
            }
        }

        fail_reason = best_fail.empty() ? "no supported repository format detected" : best_fail;
        return result;
    }
}
