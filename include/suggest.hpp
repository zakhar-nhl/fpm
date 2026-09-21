#ifndef SUGGEST_HPP
#define SUGGEST_HPP

#include <string>
#include <vector>

class Suggest
{
public:
    static std::vector<std::string> suggest_packages(const std::string &query, int max_results = 5);
};

#endif // SUGGEST_HPP
