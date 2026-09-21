#include "suggest.hpp"
#include "database.hpp"

std::vector<std::string> Suggest::suggest_packages(const std::string &query, int max_results)
{
    std::vector<std::string> results;
    auto all = DatabaseManager::get_all_packages();

    for (const auto &pkg : all)
    {
        if (results.size() >= static_cast<size_t>(max_results))
            break;

        if (pkg.name.find(query) != std::string::npos ||
            pkg.description.find(query) != std::string::npos)
        {
            results.push_back(pkg.name);
        }
    }

    return results;
}
