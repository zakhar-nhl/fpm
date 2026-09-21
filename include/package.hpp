#ifndef PACKAGE_HPP
#define PACKAGE_HPP

#include <string>
#include <vector>

// Core Operations
bool install_packages(const std::vector<std::string> &pkgs);
bool install_local_package(const std::string &fpm_path);
bool remove_packages(const std::vector<std::string> &pkgs, bool purge);
bool search_packages(const std::string &query, bool fuzzy = false, bool show_all = false);
bool search_installed_packages(const std::string &query);
bool show_package_info(const std::string &pkg);
bool list_installed_packages(const std::string &filter = "");

// Synchronization & Maintenance
bool update_db();
bool upgrade_system();
bool full_system_update();
bool autoremove_orphans(bool remove);
bool clean_cache_cmd();
bool verify_package(const std::string &pkg);
bool mirror_selector_command();
bool add_mirror_command(const std::string &url, bool active);
bool reset_mirror_command();
bool delete_mirror_command(const std::string &url);
bool news_command();
bool clean_deep_cmd();

// Build & Development
bool build_package(const std::string &dir);

// Repository backend: generate a publishable FPM repository index
// (packages.db + packages.json) from .fpm packages in <dir>.
bool make_repo(const std::string &dir, const std::string &base_url = "");

#endif // PACKAGE_HPP