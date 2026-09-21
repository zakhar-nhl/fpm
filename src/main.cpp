#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include "package.hpp"
#include "database.hpp"
#include "logger.hpp"
#include "mirror_selector.hpp"
#include "i18n.hpp"

static const char *VERSION = "0.2.0-alpha";

static std::string detect_locale()
{
    const char *lang = std::getenv("LANG");
    if (!lang)
        return "en";

    std::string l = lang;
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (l.rfind("ru", 0) == 0) return "ru";
    if (l.rfind("es", 0) == 0) return "es";
    if (l.rfind("de", 0) == 0) return "de";
    if (l.rfind("zh", 0) == 0) return "zh";
    return "en";
}

void print_help()
{
    std::cout << "Non Human Package Manager (FPM) v" << VERSION << " (x86_64)\n";
    std::cout << "Usage: fpm <option> [packages/args]\n\n";
    std::cout << "Core Operations:\n";
    std::cout << "  -i,   install <pkg...>    Install package(s)\n";
    std::cout << "  -r,   remove <pkg...>     Remove package(s), keeping configuration\n";
    std::cout << "  -p,   purge <pkg...>      Remove package(s) and configs\n";
    std::cout << "  -s,   search <query>      Search for packages by name\n";
    std::cout << "  -fs,  fsearch <query>     Deep search: name, description and fuzzy matches\n";
    std::cout << "  -qs,  qsearch <query>     Search installed packages only\n";
    std::cout << "  -info info <pkg>          Show package info\n";
    std::cout << "  -l,   list [query]        List installed packages (filter by query)\n";
    std::cout << "  -pkg  <file.fpm>          Install local .fpm package\n\n";
    std::cout << "Synchronization & Maintenance:\n";
    std::cout << "  -upd, update              Update repository index from mirrors\n";
    std::cout << "  -upg, upgrade             Upgrade all installed packages\n";
    std::cout << "  -su,  sysup               Full system update (update + upgrade)\n";
    std::cout << "  -ar,  autoremove          Remove orphaned dependencies\n";
    std::cout << "  -o,   orphans             List orphaned dependencies\n";
    std::cout << "  -c,   clean               Clear package cache\n";
    std::cout << "  -cmd, cleandisk           CleanMyDisk: cache + temporary files\n";
    std::cout << "  -v,   verify <pkg>        Verify package integrity\n\n";
    std::cout << "Build & Development:\n";
    std::cout << "  -b,   build <dir>         Build package from recipe directory\n";
    std::cout << "  -repo <dir> [--base URL]  Generate FPM repository index from .fpm packages\n\n";
    std::cout << "System & Debug:\n";
    std::cout << "  -ms,  mselect             Mirror selector (probe candidates.list, pick active mirror)\n";
    std::cout << "  -am,  add-mirror <URL>    Add a mirror to candidates.list (use --active for mirrors.list)\n";
    std::cout << "  -dm,  del-mirror <URL>    Delete a user-added mirror from the pool and active list\n";
    std::cout << "  -rm,  reset-mirror        Reset active mirror to the default fallback (geo.pkgbuild.com)\n";
    std::cout << "  -n,   news                Show FPM release notes for this version\n";
    std::cout << "  -vr,  --version           Show FPM version\n";
    std::cout << "  -h,   --help              Show this help message\n";
}

int main(int argc, char *argv[])
{
    I18n::set_locale(detect_locale());

    if (geteuid() == 0)
    {
        DatabaseManager::set_db_path("/var/lib/fpm");
        DatabaseManager::set_cache_path("/var/cache/fpm");
        Logger::init("/var/log/fpm.log");
    }
    else
    {
        DatabaseManager::set_db_path("./fpm_db");
        Logger::init("./fpm.log");
    }

    MirrorSelector::set_state_path(DatabaseManager::get_selected_mirror_path());

    if (argc < 2)
    {
        print_help();
        return 0;
    }

    std::string opt = argv[1];

    if ((opt == "-i" || opt == "install" || opt == "-r" || opt == "remove" ||
         opt == "-p" || opt == "purge" || opt == "-su" || opt == "sysup" ||
         opt == "-upg" || opt == "upgrade" || opt == "-pkg" ||
         opt == "-ar" || opt == "autoremove") &&
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
        return install_packages(args) ? 0 : 1;
    }
    else if (opt == "-r" || opt == "remove")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name(s)\n";
            return 1;
        }
        return remove_packages(args, false) ? 0 : 1;
    }
    else if (opt == "-p" || opt == "purge")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name(s)\n";
            return 1;
        }
        return remove_packages(args, true) ? 0 : 1;
    }
    else if (opt == "-s" || opt == "search" || opt == "-fs" || opt == "fsearch")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing search query\n";
            return 1;
        }
        bool fuzzy = (opt == "-fs" || opt == "fsearch");
        return search_packages(args[0], fuzzy) ? 0 : 1;
    }
    else if (opt == "-info" || opt == "info")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name\n";
            return 1;
        }
        return show_package_info(args[0]) ? 0 : 1;
    }
    else if (opt == "-qs" || opt == "qsearch")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing search query\n";
            return 1;
        }
        return search_installed_packages(args[0]) ? 0 : 1;
    }
    else if (opt == "-l" || opt == "list")
    {
        return list_installed_packages(args.empty() ? "" : args[0]) ? 0 : 1;
    }
    else if (opt == "-pkg")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing .fpm file path\n";
            return 1;
        }
        return install_local_package(args[0]) ? 0 : 1;
    }
    // SYNCHRONIZATION & MAINTENANCE
    else if (opt == "-upd" || opt == "update")
    {
        return update_db() ? 0 : 1;
    }
    else if (opt == "-upg" || opt == "upgrade")
    {
        return upgrade_system() ? 0 : 1;
    }
    else if (opt == "-su" || opt == "sysup")
    {
        return full_system_update() ? 0 : 1;
    }
    else if (opt == "-o" || opt == "-orphans" || opt == "orphans")
    {
        return autoremove_orphans(false) ? 0 : 1;
    }
    else if (opt == "-ar" || opt == "autoremove")
    {
        return autoremove_orphans(true) ? 0 : 1;
    }
    else if (opt == "-c" || opt == "clean")
    {
        return clean_cache_cmd() ? 0 : 1;
    }
    else if (opt == "-cmd" || opt == "cleandisk")
    {
        return clean_deep_cmd() ? 0 : 1;
    }
    else if (opt == "-v" || opt == "-verify" || opt == "verify")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing package name\n";
            return 1;
        }
        return verify_package(args[0]) ? 0 : 1;
    }
    // BUILD & DEVELOPMENT
    else if (opt == "-b" || opt == "build")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing recipe directory\n";
            return 1;
        }
        return build_package(args[0]) ? 0 : 1;
    }
    // REPOSITORY BACKEND
    else if (opt == "-repo" || opt == "repo")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing repository directory\n";
            return 1;
        }
        std::string base;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--base" && i + 1 < args.size())
                base = args[++i];
        }
        return make_repo(args[0], base) ? 0 : 1;
    }
    // SYSTEM & DEBUG
    else if (opt == "-ms" || opt == "mselect")
    {
        return mirror_selector_command() ? 0 : 1;
    }
    else if (opt == "-am" || opt == "add-mirror")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing mirror URL\n";
            return 1;
        }
        bool active = false;
        std::string url;
        for (const auto &a : args)
        {
            if (a == "--active")
                active = true;
            else if (url.empty())
                url = a;
        }
        if (url.empty())
        {
            std::cerr << "Error: Missing mirror URL\n";
            return 1;
        }
        return add_mirror_command(url, active) ? 0 : 1;
    }
    else if (opt == "-dm" || opt == "-dmm" || opt == "del-mirror" || opt == "delete")
    {
        if (args.empty())
        {
            std::cerr << "Error: Missing mirror URL\n";
            return 1;
        }
        return delete_mirror_command(args[0]) ? 0 : 1;
    }
    else if (opt == "-rm" || opt == "reset-mirror" || opt == "reset")
    {
        return reset_mirror_command() ? 0 : 1;
    }
    else if (opt == "-n" || opt == "news")
    {
        return news_command() ? 0 : 1;
    }
    else if (opt == "-vr" || opt == "version" || opt == "--version")
    {
std::cout << "[ X _ X ] Non Human Package Manager (FPM) v" << VERSION << " (x86_64)\n";
        return 0;
    }
    else if (opt == "-h" || opt == "help" || opt == "--help")
    {
        print_help();
        return 0;
    }
    else
    {
        std::cerr << "Unknown option: " << opt << "\nUse 'fpm -h' for help.\n";
        return 1;
    }

    return 0;
}