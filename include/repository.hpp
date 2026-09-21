#ifndef REPOSITORY_HPP
#define REPOSITORY_HPP

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>
#include "database.hpp"

namespace repo
{
    using FileFetcher = std::function<bool(const std::string &abs_url,
                                          const std::string &local_path,
                                          int timeout_ms)>;

    struct AdapterOutcome
    {
        bool ok = false;
        std::vector<RepoPackage> packages;
        std::string source;
        std::string note;
        int note_prio = 0;
    };

    class RepoAdapter
    {
    public:
        virtual ~RepoAdapter() = default;
        virtual std::string format_name() const = 0;
        // max_packages == 0 means "no limit" (parse the full index).
        virtual AdapterOutcome probe(const std::string &base,
                                     const FileFetcher &fetch,
                                     size_t max_packages = 0) const = 0;
    };

    std::unique_ptr<RepoAdapter> make_fpm_adapter();
    std::unique_ptr<RepoAdapter> make_arch_adapter();
    std::unique_ptr<RepoAdapter> make_debian_adapter();
    std::unique_ptr<RepoAdapter> make_alpine_adapter();
    std::unique_ptr<RepoAdapter> make_rpm_adapter();

    std::vector<std::unique_ptr<RepoAdapter>> default_adapters();

    struct ProbeResult
    {
        bool ok = false;
        std::string format;
        std::string source;
        std::vector<RepoPackage> packages;
    };

    ProbeResult probe_mirror(const std::string &base, std::string &fail_reason,
                             int timeout_ms = 15000, size_t max_packages = 0);

    std::string normalize_dep_name(std::string dep);
    bool looks_like_sha256(const std::string &s);
    std::string deptool_name();
}

#endif // REPOSITORY_HPP
