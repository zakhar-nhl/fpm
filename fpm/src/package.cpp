#include "../include/package.hpp"
#include "../include/logger.hpp"
#include "../include/database.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

const std::string BOLD = "\033[1m";
const std::string RESET = "\033[0m";
const std::string GREEN = "\033[32m";
const std::string CYAN = "\033[36m";
const std::string RED = "\033[31m";

void print_table_header()
{
    std::cout << std::left
              << std::setw(32) << "Package"
              << std::setw(12) << "Arch."
              << std::setw(28) << "Version"
              << std::setw(20) << "Repository"
              << std::setw(10) << "Size"
              << "\n";
}

void print_table_row(const std::string &name, const std::string &arch, const std::string &version, const std::string &repo, const std::string &size)
{
    std::cout << " "
              << std::left
              << std::setw(31) << name
              << std::setw(12) << arch
              << std::setw(28) << version
              << std::setw(20) << repo
              << std::setw(10) << size
              << "\n";
}

void print_step(int current, int total, const std::string &action, const std::string &target, const std::string &speed, const std::string &size, const std::string &time_str)
{
    std::string step_label = "[" + std::to_string(current) + "/" + std::to_string(total) + "] " + action + " " + target;

    std::cout << std::left << std::setw(75) << step_label
              << "100% | " << std::right << std::setw(10) << speed
              << " | " << std::setw(10) << size
              << " | " << std::setw(6) << time_str << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

bool ask_confirmation()
{
    std::cout << "\n"
              << BOLD << "Is this ok [y/N]: " << RESET;
    std::string choice;
    if (!(std::cin >> choice))
        return false;
    return (choice == "y" || choice == "Y" || choice == "yes" || choice == "YES");
}

// --- CORE OPERATIONS ---

bool install_packages(const std::vector<std::string> &pkgs)
{
    std::cout << BOLD << "Updating and loading repositories:" << RESET << "\n";
    std::cout << "Repositories loaded.\n\n";

    print_table_header();

    std::cout << BOLD << "Installing:" << RESET << "\n";
    for (const auto &pkg : pkgs)
    {
        if (DatabaseManager::is_installed(pkg))
        {
            std::cout << "Package " << pkg << " is already installed.\n";
            return false;
        }
        print_table_row(pkg, "x86_64", "0:1.0.0-1.nhl", "core", "1.6 MiB");
    }

    std::cout << "\n"
              << BOLD << "Transaction Summary:" << RESET << "\n";
    std::cout << " Installing:         " << pkgs.size() << " package(s)\n\n";

    if (!ask_confirmation())
    {
        std::cout << "Operation aborted.\n";
        return false;
    }

    std::cout << "\n"
              << BOLD << "Running transaction" << RESET << "\n";
    int steps = pkgs.size() + 2;
    int cur = 1;

    print_step(cur++, steps, "Verify package files", "", "250.0   B/s", "2.0   B", "00m00s");
    print_step(cur++, steps, "Prepare transaction", "", "7.0   B/s", "2.0   B", "00m00s");

    for (const auto &pkg_name : pkgs)
    {
        print_step(cur++, steps, "Installing", pkg_name + "-0:1.0.0-1.nhl.x86_64", "3.9 MiB/s", "1.6 MiB", "00m00s");

        // Запись в реальную БД
        InstalledPackageInfo pkg;
        pkg.name = pkg_name;
        pkg.version = "1.0.0-1.nhl";
        pkg.arch = "x86_64";
        pkg.repo = "core";
        pkg.size = "1.6 MiB";
        pkg.description = "Non Human Linux standard package";
        pkg.files = {"/usr/bin/" + pkg_name, "/usr/share/doc/" + pkg_name + "/README"};

        DatabaseManager::add_package(pkg);
    }

    std::cout << GREEN << BOLD << "Complete!" << RESET << "\n";
    Logger::info("Successfully installed package(s).");
    return true;
}

bool remove_packages(const std::vector<std::string> &pkgs, bool purge)
{
    print_table_header();

    std::cout << BOLD << (purge ? "Purging:" : "Removing:") << RESET << "\n";
    std::vector<std::string> to_remove;

    for (const auto &pkg : pkgs)
    {
        if (!DatabaseManager::is_installed(pkg))
        {
            std::cout << "Package " << pkg << " is not installed.\n";
            continue;
        }
        to_remove.push_back(pkg);
        print_table_row(pkg, "x86_64", "0:1.0.0-1.nhl", "@system", "1.5 MiB");
    }

    if (to_remove.empty())
    {
        std::cout << "Nothing to do.\n";
        return false;
    }

    std::cout << "\n"
              << BOLD << "Transaction Summary:" << RESET << "\n";
    std::cout << " " << (purge ? "Purging" : "Removing") << ":           " << to_remove.size() << " package(s)\n\n";

    if (!ask_confirmation())
    {
        std::cout << "Operation aborted.\n";
        return false;
    }

    std::cout << "\n"
              << BOLD << "Running transaction" << RESET << "\n";
    int steps = to_remove.size() + 1;
    int cur = 1;

    print_step(cur++, steps, "Prepare transaction", "", "11.0   B/s", "2.0   B", "00m00s");
    for (const auto &pkg : to_remove)
    {
        print_step(cur++, steps, "Removing", pkg + "-0:1.0.0-1.nhl.x86_64", "2.0 KiB/s", "52.0   B", "00m00s");
        DatabaseManager::remove_package(pkg);
    }

    std::cout << GREEN << BOLD << "Complete!" << RESET << "\n";
    Logger::info("Successfully removed package(s).");
    return true;
}

bool search_packages(const std::string &query)
{
    std::cout << BOLD << "Matching packages for '" << query << "':" << RESET << "\n\n";
    print_table_header();
    print_table_row(query, "x86_64", "0:2.1.0-1.nhl", "core", "3.4 MiB");
    return true;
}

bool show_package_info(const std::string &pkg_name)
{
    InstalledPackageInfo pkg;
    if (DatabaseManager::get_package(pkg_name, pkg))
    {
        std::cout << BOLD << "Name         : " << RESET << pkg.name << "\n"
                  << BOLD << "Version      : " << RESET << pkg.version << "\n"
                  << BOLD << "Architecture : " << RESET << pkg.arch << "\n"
                  << BOLD << "Repository   : " << RESET << pkg.repo << "\n"
                  << BOLD << "Size         : " << RESET << pkg.size << "\n"
                  << BOLD << "Description  : " << RESET << pkg.description << "\n"
                  << BOLD << "Files        : " << RESET << "\n";
        for (const auto &f : pkg.files)
        {
            std::cout << "  " << f << "\n";
        }
    }
    else
    {
        std::cout << RED << "Package '" << pkg_name << "' is not installed or not found in DB." << RESET << "\n";
    }
    return true;
}

bool list_installed_packages()
{
    auto packages = DatabaseManager::get_all_packages();
    if (packages.empty())
    {
        std::cout << "No packages currently installed.\n";
        return true;
    }

    std::cout << BOLD << "Installed packages:" << RESET << "\n\n";
    print_table_header();
    for (const auto &pkg : packages)
    {
        print_table_row(pkg.name, pkg.arch, pkg.version, "@system", pkg.size);
    }
    return true;
}

// --- SYNCHRONIZATION & MAINTENANCE ---

bool update_db()
{
    std::cout << BOLD << "Updating and loading repositories:" << RESET << "\n";
    print_step(1, 2, "Fetching", "core.db", "1.2 MiB/s", "450.0 KiB", "00m00s");
    print_step(2, 2, "Fetching", "extra.db", "2.1 MiB/s", "1.2 MiB", "00m00s");
    std::cout << GREEN << BOLD << "Repositories updated successfully." << RESET << "\n";
    return true;
}

bool upgrade_system()
{
    std::cout << BOLD << "Checking for package upgrades..." << RESET << "\n";
    std::cout << "Nothing to do. System is fully upgraded.\n";
    return true;
}

bool full_system_update()
{
    update_db();
    upgrade_system();
    return true;
}

bool autoremove_orphans()
{
    std::cout << BOLD << "Checking for orphaned dependencies..." << RESET << "\n";
    std::cout << "No orphaned packages found to remove.\n";
    return true;
}

bool clean_cache()
{
    std::cout << BOLD << "Clearing package archive cache..." << RESET << "\n";
    print_step(1, 1, "Cleaning", "/var/cache/fpm/pkg/", "100 MiB/s", "45.0 MiB", "00m00s");
    std::cout << GREEN << BOLD << "Cache cleared!" << RESET << "\n";
    return true;
}

bool verify_package(const std::string &pkg)
{
    if (DatabaseManager::is_installed(pkg))
    {
        std::cout << BOLD << "Verifying integrity of package: " << pkg << RESET << "\n";
        print_step(1, 1, "Checking SHA256", pkg, "450 MiB/s", "1.6 MiB", "00m00s");
        std::cout << GREEN << BOLD << "Package integrity verified: OK" << RESET << "\n";
    }
    else
    {
        std::cout << RED << "Package '" << pkg << "' is not installed." << RESET << "\n";
    }
    return true;
}

// --- BUILD & DEVELOPMENT ---

bool build_package(const std::string &dir)
{
    std::cout << BOLD << "Building FPM package from recipe directory: " << dir << RESET << "\n";
    print_step(1, 3, "Parsing recipe", dir + "/fpm.recipe", "1.0 KiB/s", "512 B", "00m00s");
    print_step(2, 3, "Compiling sources", dir, "12.4 MiB/s", "4.2 MiB", "00m01s");
    print_step(3, 3, "Archiving package", "package.fpm", "45.0 MiB/s", "1.6 MiB", "00m00s");
    std::cout << GREEN << BOLD << "Complete! Package built successfully." << RESET << "\n";
    return true;
}
