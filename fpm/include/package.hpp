#ifndef PACKAGE_HPP
#define PACKAGE_HPP

#include <string>
#include <vector>

struct Package
{
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> files;
};

// Core Operations
bool install_packages(const std::vector<std::string> &pkgs);
bool remove_packages(const std::vector<std::string> &pkgs, bool purge);
bool search_packages(const std::string &query);
bool show_package_info(const std::string &pkg);
bool list_installed_packages();

// Synchronization & Maintenance
bool update_db();
bool upgrade_system();
bool full_system_update();
bool autoremove_orphans();
bool clean_cache();
bool verify_package(const std::string &pkg);

// Build & Development
bool build_package(const std::string &dir);

#endif // PACKAGE_HPP