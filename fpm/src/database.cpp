#include "../include/database.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

std::string DatabaseManager::db_dir = "./fpm_db";

void DatabaseManager::set_db_path(const std::string &path)
{
    db_dir = path;
}

std::string DatabaseManager::get_db_path()
{
    return db_dir;
}

bool DatabaseManager::init()
{
    try
    {
        if (!fs::exists(db_dir))
        {
            fs::create_directories(db_dir);
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Database Error: " << e.what() << std::endl;
        return false;
    }
}

bool DatabaseManager::is_installed(const std::string &name)
{
    fs::path pkg_file = fs::path(db_dir) / (name + ".db");
    return fs::exists(pkg_file);
}

bool DatabaseManager::add_package(const InstalledPackageInfo &pkg)
{
    init();
    fs::path pkg_file = fs::path(db_dir) / (pkg.name + ".db");
    std::ofstream out(pkg_file);
    if (!out.is_open())
        return false;

    out << "NAME=" << pkg.name << "\n";
    out << "VERSION=" << pkg.version << "\n";
    out << "ARCH=" << pkg.arch << "\n";
    out << "REPO=" << pkg.repo << "\n";
    out << "SIZE=" << pkg.size << "\n";
    out << "DESC=" << pkg.description << "\n";
    out << "FILES:\n";
    for (const auto &file : pkg.files)
    {
        out << file << "\n";
    }
    return true;
}

bool DatabaseManager::remove_package(const std::string &name)
{
    fs::path pkg_file = fs::path(db_dir) / (name + ".db");
    if (fs::exists(pkg_file))
    {
        return fs::remove(pkg_file);
    }
    return false;
}

bool DatabaseManager::get_package(const std::string &name, InstalledPackageInfo &pkg)
{
    fs::path pkg_file = fs::path(db_dir) / (name + ".db");
    if (!fs::exists(pkg_file))
        return false;

    std::ifstream in(pkg_file);
    if (!in.is_open())
        return false;

    std::string line;
    bool reading_files = false;
    pkg.files.clear();

    while (std::getline(in, line))
    {
        if (reading_files)
        {
            if (!line.empty())
                pkg.files.push_back(line);
            continue;
        }

        if (line == "FILES:")
        {
            reading_files = true;
            continue;
        }

        size_t eq = line.find('=');
        if (eq != std::string::npos)
        {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "NAME")
                pkg.name = val;
            else if (key == "VERSION")
                pkg.version = val;
            else if (key == "ARCH")
                pkg.arch = val;
            else if (key == "REPO")
                pkg.repo = val;
            else if (key == "SIZE")
                pkg.size = val;
            else if (key == "DESC")
                pkg.description = val;
        }
    }
    return true;
}

std::vector<InstalledPackageInfo> DatabaseManager::get_all_packages()
{
    init();
    std::vector<InstalledPackageInfo> list;
    for (const auto &entry : fs::directory_iterator(db_dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".db")
        {
            std::string name = entry.path().stem().string();
            InstalledPackageInfo pkg;
            if (get_package(name, pkg))
            {
                list.push_back(pkg);
            }
        }
    }
    return list;
}
