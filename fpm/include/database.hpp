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
    std::vector<std::string> files;
};

class DatabaseManager
{
public:
    static void set_db_path(const std::string &path);
    static std::string get_db_path();
    static bool init();
    static bool is_installed(const std::string &name);
    static bool add_package(const InstalledPackageInfo &pkg);
    static bool remove_package(const std::string &name);
    static bool get_package(const std::string &name, InstalledPackageInfo &pkg);
    static std::vector<InstalledPackageInfo> get_all_packages();

private:
    static std::string db_dir;
};

#endif // DATABASE_HPP
