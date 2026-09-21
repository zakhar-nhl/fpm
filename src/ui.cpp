#include "ui.hpp"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
 #include <cstring>

namespace UI
{

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

void print_table_row(const std::string &name, const std::string &arch,
                     const std::string &version, const std::string &repo,
                     const std::string &size)
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

void print_step(int current, int total, const std::string &action,
                const std::string &target, const std::string &speed,
                const std::string &size, const std::string &time_str)
{
    std::string step_label = "[" + std::to_string(current) + "/" +
                             std::to_string(total) + "] " + action + " " + target;

    std::cout << std::left << std::setw(75) << step_label
              << "100% | " << std::right << std::setw(10) << speed
              << " | " << std::setw(10) << size
              << " | " << std::setw(6) << time_str << "\n";
}

bool ask_confirmation(const std::string &prompt)
{
    std::cout << "\n"
              << BOLD << prompt << RESET;
    std::string choice;
    if (!(std::cin >> choice))
        return false;
    return (choice == "y" || choice == "Y" || choice == "yes" || choice == "YES");
}

std::string format_size(uint64_t bytes)
{
    std::ostringstream ss;
    if (bytes < 1024)
        ss << bytes << " B";
    else if (bytes < 1024 * 1024)
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KiB";
    else if (bytes < 1024ULL * 1024 * 1024)
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
    else
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
    return ss.str();
}

std::string format_ms(int ms)
{
    if (ms < 0)
        return "--";
    if (ms < 1000)
        return std::to_string(ms) + " ms";
    return std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 100) + " s";
}

std::string read_line(const std::string &prompt)
{
    std::cout << prompt;
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line))
        return "";

    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

int prompt_number(const std::string &prompt, int max_value)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::string line = read_line(prompt);
        if (line.empty())
            return 0;

        try
        {
            size_t consumed = 0;
            int value = std::stoi(line, &consumed);
            if (consumed >= line.size() && value >= 0 && value <= max_value)
                return value;
        }
        catch (...)
        {
        }

        std::cout << YELLOW << "Invalid input. Enter a number from 0 to " << max_value << "." << RESET << "\n";
    }
    return 0;
}

void print_numbered_item(int idx, const std::string &left, const std::string &right)
{
    std::cout << "  " << CYAN << "[" << BOLD << idx << RESET << CYAN << "]" << RESET
              << " " << std::left << std::setw(40) << left
              << std::right << std::setw(30) << right << "\n";
}

// ---------------------------------------------------------------------------
// Pager / fzf helpers (used by search result listing).
// ---------------------------------------------------------------------------

bool is_tty()
{
    return isatty(fileno(stdout)) != 0;
}

static bool program_available(const std::string &name)
{
    // Cheap PATH lookup so we never spawn a shell just to test existence.
    const char *path_env = getenv("PATH");
    if (!path_env)
        return false     ;
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':'))
    {
        if (dir.empty())
            dir = ".";
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

bool have_less()
{
    static const bool cached = program_available("less");
    return cached;
}

bool have_fzf()
{
    static const bool cached = program_available("fzf");
    return cached;
}

bool page_text(const std::string &text)
{
    if (!is_tty() || !have_less())
    {
        std::cout << text;
        std::cout.flush();
        return false;
    }
    FILE *pager = popen("less -R -X -F", "w");
    if (!pager)
    {
        std::cout << text;
        std::cout.flush();
        return false;
    }
    fwrite(text.data(), 1, text.size(), pager);
    int rc = pclose(pager);
    return rc == 0 || rc == 0x100; // 0x100 = LESS clears the screen (-X) fold
}

bool fzf_select(const std::string &prompt,
                const std::vector<std::string> &choices,
                std::string &selected)
{
    if (!have_fzf() || choices.empty())
        return false;

    std::string in_path  = "/tmp/fpm_fzf_in_"  + std::to_string(getpid());
    std::string out_path = "/tmp/fpm_fzf_out_" + std::to_string(getpid());

    std::string input;
    for (const auto &c : choices)
        input += c + "\n";

    FILE *in = fopen(in_path.c_str(), "w");
    if (!in)
        return false;
    fwrite(input.data(), 1, input.size(), in);
    fclose(in);

    std::string cmd = "fzf --height=40% --layout=reverse --cycle "
                      "--border --header=\"" + prompt + "\" --prompt=\"> \" "
                      " < " + in_path + " > " + out_path + " 2>/dev/null";

    int rc = system(cmd.c_str());

    std::remove(in_path.c_str());

    if (rc != 0)
    {
        std::remove(out_path.c_str());
        selected.clear();
        return false;
    }

    std::string result;
    FILE *out = fopen(out_path.c_str(), "r");
    if (out)
    {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), out)) > 0)
            result.append(buf, n);
        fclose(out);
    }
    std::remove(out_path.c_str());

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    if (result.empty())
    {
        selected.clear();
        return false;
    }

    selected = result;
    return true;
}

} // namespace UI

