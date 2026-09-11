#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include "package.hpp"
#include "database.hpp"
#include "logger.hpp"

void print_help()
{
    std::cout << "Non Human Package Manager (FPM) v0.1.0-alpha (x86_64)\n";
    std::cout << "Usage: fpm <option> [packages/args]\n\n";
    std::cout << "Core Operations:\n";
    std::cout << "  -i,  install <pkg...>   Install package(s)\n";
    std::cout << "  -r,  remove <pkg...>    Remove package(s)\n";
    std::cout << "  -p,  purge <pkg...>     Remove package(s) and configs\n";
    std::cout << "  -s,  search <query>     Search for packages\n";
    std::cout << "  -info info <pkg>        Show package info\n";
    std::cout << "  -l,  list               List installed packages\n\n";
    std::cout << "Synchronization & Maintenance:\n";
    std::cout << "  -upd update             Update package database\n";
    std::cout << "  -upg upgrade            Upgrade system packages\n";
    std::cout << "  -su, sysup              Full system update\n";
    std::cout << "  -ar, autoremove         Remove unused orphan packages\n";
    std::cout << "  -c,  clean              Clean package cache\n";
    std::cout << "  -verify <pkg>           Verify package integrity\n\n";
    std::cout << "Build & Development:\n";
    std::cout << "  -b,  build <dir>        Build package from recipe directory\n";
    std::cout << "  -v,  version            Show FPM version\n";
    std::cout << "  -h,  help               Show this help message\n";
}

int main(int argc, char *argv[])
{
    // Проверка прав и установка путей
    if (geteuid() == 0)
    {
        DatabaseManager::set_db_path("/var/lib/fpm");
        Logger::init("/var/log/fpm.log");
    }
    else
    {
        DatabaseManager::set_db_path("./fpm_db");
        Logger::init("./fpm.log");
    }

    if (argc < 2)
    {
        print_help();
        return 1;
    }

    std::string opt = argv[1];

    // Проверка прав root для системных действий
    if ((opt == "-i" || opt == "install" || opt == "-r" || opt == "remove" ||
         opt == "-p" || opt == "purge" || opt == "-su" || opt == "sysup") &&
        geteuid() != 0)
    {
        std::cerr << "Error: This operation requires root privileges. Try running with sudo.\n";
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i)
    {
        args.push_back(argv[i]);
    }

    // CORE OPERATIONS
    if (opt == "-i" || opt == "install")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name(s)\n";
            return 1;
        }
        install_packages(args);
    }
    else if (opt == "-r" || opt == "remove")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name(s)\n";
            return 1;
        }
        remove_packages(args, false);
    }
    else if (opt == "-p" || opt == "purge")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name(s)\n";
            return 1;
        }
        remove_packages(args, true);
    }
    else if (opt == "-s" || opt == "search")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing search query\n";
            return 1;
        }
        search_packages(args[0]);
    }
    else if (opt == "-info" || opt == "info")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name\n";
            return 1;
        }
        show_package_info(args[0]);
    }
    else if (opt == "-l" || opt == "list")
    {
        list_installed_packages();
    }
    // SYNCHRONIZATION & MAINTENANCE
    else if (opt == "-upd" || opt == "update")
    {
        update_db();
    }
    else if (opt == "-upg" || opt == "upgrade")
    {
        upgrade_system();
    }
    else if (opt == "-su" || opt == "sysup")
    {
        full_system_update();
    }
    else if (opt == "-ar" || opt == "autoremove")
    {
        autoremove_orphans();
    }
    else if (opt == "-c" || opt == "clean")
    {
        clean_cache();
    }
    else if (opt == "-verify")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name\n";
            return 1;
        }
        verify_package(args[0]);
    }
    // BUILD & DEVELOPMENT
    else if (opt == "-b" || opt == "build")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing recipe directory\n";
            return 1;
        }
        build_package(args[0]);
    }
    // SYSTEM & DEBUG
    else if (opt == "-v" || opt == "version")
    {
        std::cout << "Non Human Package Manager (FPM) v0.1.0-alpha (x86_64)\n";
    }
    else if (opt == "-h" || opt == "help")
    {
        print_help();
    }
    else
    {
        std::cerr << "Unknown option: " << opt << "\nUse 'fpm -h' for help.\n";
        return 1;
    }

    return 0;
}
