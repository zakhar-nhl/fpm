#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <string>
#include <vector>

struct InstalledPackageInfo
{
    std::string name;
    std::string version;
    std::string arch;
    std::string repo;
    std::string size;
    std::string description;
    std::string sha256;
    bool explicit_install = true;
    std::vector<std::string> files;
    std::vector<std::string> file_hashes;
};

struct RepoPackage
{
    std::string name;
    std::string version;
    std::string arch;
    std::string repo;
    std::string size;
    std::string description;
    std::string sha256;
    std::vector<std::string> depends;
    std::string url;
};

class DatabaseManager
{
public:
    static void set_db_path(const std::string &path);
    static void set_cache_path(const std::string &path);
    static std::string get_db_path();
    static std::string get_local_path();
    static std::string get_repo_cache_path();
    static std::string get_package_cache_path();
    static bool init();

    // Local installed package DB
    static bool is_installed(const std::string &name);
    static bool add_package(const InstalledPackageInfo &pkg);
    static bool remove_package(const std::string &name);
    static bool get_package(const std::string &name, InstalledPackageInfo &pkg);
    static std::vector<InstalledPackageInfo> get_all_packages();
    static std::vector<std::string> get_installed_files(const std::string &name);

    // Repo DB cache
    static bool save_repo_index(const std::string &repo_name, const std::string &content);
    static bool load_repo_index(const std::string &repo_name, std::string &content);
    static std::vector<RepoPackage> search_repo(const std::string &query, bool fuzzy = false);
    static bool get_repo_package(const std::string &name, RepoPackage &pkg);
    static std::vector<RepoPackage> get_repo_packages(const std::string &repo = "");

    // Unified remote index (remote_packages.db)
    static bool save_remote_index(const std::vector<RepoPackage> &packages);
    static bool save_repo_splits(const std::vector<RepoPackage> &packages);
    static std::vector<RepoPackage> load_remote_index();
    static std::string get_remote_index_path();
    static std::string get_selected_mirror_path();

private:
    static std::string db_dir;
    static std::string cache_dir;
    static std::string install_meta_path(const std::string &name);
    static std::string install_manifest_path(const std::string &name);
    static bool write_meta(const std::string &path, const InstalledPackageInfo &pkg);
    static bool read_meta(const std::string &path, InstalledPackageInfo &pkg);
    static bool write_manifest(const std::string &path, const std::vector<std::string> &files,
                               const std::vector<std::string> &file_hashes);
    static bool read_manifest(const std::string &path, std::vector<std::string> &files,
                              std::vector<std::string> &file_hashes);
};

#endif // DATABASE_HPP
