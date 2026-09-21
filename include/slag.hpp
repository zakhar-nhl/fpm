#ifndef SLAG_HPP
#define SLAG_HPP

#include <string>
#include <vector>
#include <cstdint>

// SLAG-AR: Simple Lightweight Archive & Generated-manifest Architecture for Recipes
//
// A .fpm package is a tar archive containing:
//   fpm.meta     - Key=Value metadata (name, version, arch, deps, sha256...)
//   fpm.manifest - File manifest: "<mode> <size> <sha256> <path>"
//   <file tree>  - Actual package files relative to root
//
// System tools used (zero C++ dependencies):
//   tar   - archive creation/extraction
//   sha256sum - integrity verification

struct PackageMeta
{
    std::string name;
    std::string version;
    std::string arch;
    std::string repo;
    std::string size;
    std::string description;
    std::string sha256;
    std::vector<std::string> depends;
};

struct ManifestEntry
{
    std::string path;
    std::string sha256;
    uint64_t size;
    std::string mode;
};

class SlagArchive
{
public:
    static bool create(const std::string &source_dir, const std::string &output_fpm);
    static bool extract(const std::string &fpm_path, const std::string &dest_dir);
    static bool read_meta_from_file(const std::string &meta_path, PackageMeta &meta);
    static bool read_meta_from_archive(const std::string &fpm_path, PackageMeta &meta);
    static bool read_manifest_from_archive(const std::string &fpm_path, std::vector<ManifestEntry> &entries);
    static bool verify_archive(const std::string &fpm_path);
    static std::string compute_sha256(const std::string &file_path);
    static std::string compute_file_sha256(const std::string &file_path);

private:
    static bool parse_meta_line(const std::string &line, PackageMeta &meta);
    static bool parse_manifest_line(const std::string &line, ManifestEntry &entry);
    static bool run_command(const std::string &cmd, std::string *output = nullptr);
};

#endif // SLAG_HPP
