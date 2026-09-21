#ifndef CLI_HPP
#define CLI_HPP

#include <string>
#include <vector>

struct CliArgs
{
    std::string command;
    std::vector<std::string> targets;
    bool help = false;
    bool version = false;
    bool purge = false;
};

class CliParser
{
public:
    static CliArgs parse(int argc, char *argv[]);
    static void print_help();
    static void print_version();
};

#endif // CLI_HPP
