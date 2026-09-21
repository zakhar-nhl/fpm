#ifndef CLEANER_HPP
#define CLEANER_HPP

#include <string>
#include <cstdint>
#include <vector>

struct CleanStats
{
    uint64_t files_removed;
    uint64_t bytes_freed;
};

class Cleaner
{
public:
    static CleanStats clean_cache();
    static CleanStats clean_package_cache();
    static uint64_t get_cache_size();

    // CleanMyDisk deep clean
    static std::vector<std::string> clean_deep(uint64_t &bytes_freed_out);
    static uint64_t remove_tmp_dirs(const std::string &pattern);

private:
    static uint64_t remove_directory_contents(const std::string &dir);
};

#endif // CLEANER_HPP
