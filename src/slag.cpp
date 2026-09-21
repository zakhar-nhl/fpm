#include "slag.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static bool is_hex_sha256(const std::string &s)
{
    if (s.size() != 64)
        return false;
    for (char c : s)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool SlagArchive::run_command(const std::string &cmd, std::string *output)
{
    std::array<char, 256> buffer;
    std::string result;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return false;

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
    {
        if (output)
            result += buffer.data();
    }

    int status = pclose(pipe);
    if (output)
        *output = result;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string SlagArchive::compute_sha256(const std::string &file_path)
{
    std::string cmd = "sha256sum " + shell_quote(file_path) + " 2>/dev/null";
    std::string output;
    if (!run_command(cmd, &output))
        return "";

    size_t space = output.find(' ');
    if (space != std::string::npos)
        return output.substr(0, space);
    return "";
}

std::string SlagArchive::compute_file_sha256(const std::string &file_path)
{
    return compute_sha256(file_path);
}

bool SlagArchive::parse_meta_line(const std::string &line, PackageMeta &meta)
{
    if (line.empty() || line[0] == '#')
        return false;

    size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;

    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);

    if (key == "NAME")
        meta.name = val;
    else if (key == "VERSION")
        meta.version = val;
    else if (key == "ARCH")
        meta.arch = val;
    else if (key == "REPO")
        meta.repo = val;
    else if (key == "SIZE")
        meta.size = val;
    else if (key == "DESC")
        meta.description = val;
    else if (key == "SHA256")
        meta.sha256 = val;
    else if (key == "DEPENDS")
    {
        std::istringstream ss(val);
        std::string dep;
        while (std::getline(ss, dep, ','))
        {
            if (!dep.empty())
                meta.depends.push_back(dep);
        }
    }
    else
        return false;

    return true;
}

bool SlagArchive::parse_manifest_line(const std::string &line, ManifestEntry &entry)
{
    if (line.empty() || line[0] == '#')
        return false;

    std::istringstream ss(line);
    std::string size_str;

    if (!(ss >> entry.mode >> size_str >> entry.sha256))
        return false;

    entry.size = std::stoull(size_str);

    std::string rest;
    std::getline(ss, rest);

    while (!rest.empty() && rest[0] == ' ')
        rest.erase(0, 1);

    entry.path = rest;
    return !entry.path.empty();
}

// Walks source_dir building both the tar member list and the fpm.manifest text.
// Member names carry no leading "./" so tar -O fpm.meta lookups work.
static bool collect_tree(const fs::path &source_dir, const fs::path &abs_output,
                         std::vector<std::string> &members, std::string &manifest_text)
{
    fs::path manifest_path = source_dir / "fpm.manifest";
    std::ofstream mf(manifest_path);
    if (!mf.is_open())
        return false;

    std::error_code ec;
    std::ostringstream text;

    for (auto it = fs::recursive_directory_iterator(source_dir, ec);
         it != fs::recursive_directory_iterator(); ++it)
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        std::error_code local_ec;
        const fs::path p = it->path();

        bool is_dir = fs::is_directory(p, local_ec);
        if (!is_dir && !fs::is_regular_file(p, local_ec))
            continue;

        std::string rel = fs::relative(p, source_dir, local_ec).generic_string();
        if (local_ec)
            continue;
        if (fs::absolute(p).lexically_normal() == abs_output)
            continue;

        members.push_back(rel);

        if (is_dir)
            continue;
        if (rel == "fpm.meta" || rel == "fpm.manifest")
            continue;

        struct stat st{};
        if (::stat(p.c_str(), &st) != 0)
            continue;

        uintmax_t sz = fs::file_size(p, local_ec);
        if (local_ec)
            continue;

        std::string sha = SlagArchive::compute_sha256(p.string());
        if (sha.empty())
            continue;

        std::string mode = "100" + [](mode_t m) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%o", static_cast<unsigned>(m & 07777));
            return std::string(buf);
        }(st.st_mode);

        std::string arch_path = "/" + rel;
        std::string line = mode + " " + std::to_string(sz) + " " + sha + " " + arch_path + "\n";
        mf << line;
        text << line;
    }

    mf.close();
    manifest_text = text.str();
    return !members.empty();
}

bool SlagArchive::create(const std::string &source_dir, const std::string &output_fpm)
{
    fs::path src(source_dir);
    fs::path meta_file = src / "fpm.meta";

    if (!fs::is_regular_file(meta_file))
    {
        std::cerr << "Error: " << meta_file.string() << " not found\n";
        return false;
    }

    fs::path abs_output = fs::absolute(output_fpm).lexically_normal();
    fs::create_directories(abs_output.parent_path());

    std::vector<std::string> members;
    std::string manifest_text;
    if (!collect_tree(src, abs_output, members, manifest_text))
    {
        std::cerr << "Error: failed to generate fpm.manifest (no package files found)\n";
        return false;
    }

    // SHA256 in fpm.meta = digest of fpm.manifest (deterministic package fingerprint).
    std::string manifest_hash = compute_sha256((src / "fpm.manifest").string());
    if (manifest_hash.empty())
    {
        std::cerr << "Error: failed to hash fpm.manifest\n";
        return false;
    }

    // Update SHA256 field in fpm.meta (add if missing, otherwise replace).
    {
        std::string meta_path_str = meta_file.string();
        std::ifstream in(meta_path_str);
        std::ostringstream buf;
        buf << in.rdbuf();
        in.close();

        std::istringstream src_lines(buf.str());
        std::ostringstream out;
        std::string line;
        bool replaced = false;
        while (std::getline(src_lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("SHA256=", 0) == 0)
            {
                out << "SHA256=" << manifest_hash << "\n";
                replaced = true;
            }
            else
            {
                out << line << "\n";
            }
        }
        if (!replaced)
            out << "SHA256=" << manifest_hash << "\n";

        std::ofstream out_file(meta_path_str, std::ios::trunc);
        if (!out_file.is_open())
        {
            std::cerr << "Error: failed to write " << meta_path_str << "\n";
            return false;
        }
        out_file << out.str();
        out_file.close();
    }

    std::string list_path = std::string("/tmp/fpm_members_") + std::to_string(static_cast<long>(getpid()));
    {
        std::ofstream lists(list_path);
        if (!lists.is_open())
        {
            std::cerr << "Error: failed to create member list\n";
            return false;
        }
        for (const auto &m : members)
            lists << m << "\n";
        lists.close();
    }

    std::string cmd = "tar --no-recursion -C " + shell_quote(src.string()) + " -cf " +
                      shell_quote(abs_output.string()) + " -T " + shell_quote(list_path) + " 2>/dev/null";
    bool ok = run_command(cmd);
    std::error_code ec;
    fs::remove(list_path, ec);

    if (!ok)
    {
        std::cerr << "Error: tar failed while creating " << abs_output.string() << "\n";
        return false;
    }

    return fs::exists(abs_output);
}

bool SlagArchive::extract(const std::string &fpm_path, const std::string &dest_dir)
{
    std::string cmd = "mkdir -p " + shell_quote(dest_dir) + " && tar xf " + shell_quote(fpm_path) +
                      " --no-same-owner --no-same-permissions -C " + shell_quote(dest_dir) + " 2>/dev/null";
    return run_command(cmd);
}

bool SlagArchive::read_meta_from_file(const std::string &meta_path, PackageMeta &meta)
{
    std::ifstream in(meta_path);
    if (!in.is_open())
        return false;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        parse_meta_line(line, meta);
    }
    return !meta.name.empty();
}

bool SlagArchive::read_meta_from_archive(const std::string &fpm_path, PackageMeta &meta)
{
    std::string cmd = "tar xf " + shell_quote(fpm_path) + " -O fpm.meta 2>/dev/null";
    std::string output;
    if (!run_command(cmd, &output))
        return false;

    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        parse_meta_line(line, meta);
    }
    return !meta.name.empty();
}

bool SlagArchive::read_manifest_from_archive(const std::string &fpm_path, std::vector<ManifestEntry> &entries)
{
    entries.clear();
    std::string cmd = "tar xf " + shell_quote(fpm_path) + " -O fpm.manifest 2>/dev/null";
    std::string output;
    if (!run_command(cmd, &output))
        return false;

    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        ManifestEntry entry;
        if (parse_manifest_line(line, entry))
            entries.push_back(entry);
    }
    return true;
}

bool SlagArchive::verify_archive(const std::string &fpm_path)
{
    PackageMeta meta;
    if (!read_meta_from_archive(fpm_path, meta))
        return false;

    std::vector<ManifestEntry> entries;
    if (!read_manifest_from_archive(fpm_path, entries) || entries.empty())
        return false;

    // Paths must be absolute and free of path traversal.
    for (const auto &e : entries)
    {
        if (e.path.empty() || e.path[0] != '/')
            return false;
        std::istringstream iss(e.path);
        std::string seg;
        while (std::getline(iss, seg, '/'))
        {
            if (seg == "..")
                return false;
        }
        if (!is_hex_sha256(e.sha256))
            return false;
    }
    return true;
}