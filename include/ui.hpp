#ifndef UI_HPP
#define UI_HPP

#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <cstdint>

namespace UI
{
    const std::string BOLD   = "\033[1m";
    const std::string RESET  = "\033[0m";
    const std::string GREEN  = "\033[32m";
    const std::string CYAN   = "\033[36m";
    const std::string RED    = "\033[31m";
    const std::string YELLOW = "\033[33m";
    const std::string DIM    = "\033[2m";

    void print_table_header();
    void print_table_row(const std::string &name, const std::string &arch,
                         const std::string &version, const std::string &repo,
                         const std::string &size);
    void print_step(int current, int total, const std::string &action,
                    const std::string &target, const std::string &speed,
                    const std::string &size, const std::string &time_str);
    bool ask_confirmation(const std::string &prompt = "Is this ok [y/N]: ");
    std::string format_size(uint64_t bytes);
    std::string format_ms(int ms);

    std::string read_line(const std::string &prompt);
    int prompt_number(const std::string &prompt, int max_value);
    void print_numbered_item(int idx, const std::string &left, const std::string &right);

    // Result-listing helpers.
    bool is_tty();                            /* true when stdout is a terminal */
    bool have_less();                         /* true when `less` exists in PATH */
    bool have_fzf();                          /* true when `fzf` exists in PATH */
    bool page_text(const std::string &text);  /* pipe through `less -R` when possible */
    bool fzf_select(const std::string &prompt,
                    const std::vector<std::string> &choices,
                    std::string &selected);   /* popen fzf; returns choice on Enter */
}

#endif // UI_HPP
