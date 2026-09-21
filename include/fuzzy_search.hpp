#ifndef FUZZY_SEARCH_HPP
#define FUZZY_SEARCH_HPP

#include <string>
#include <vector>

struct SearchResult
{
    std::string name;
    int score;
};

class FuzzySearch
{
public:
    static std::vector<SearchResult> search(const std::string &query,
                                            const std::vector<std::string> &candidates,
                                            int max_results = 10);
    // Damerau-Levenshtein distance (adjacent transpositions cost 1 edit).
    static int distance(const std::string &a, const std::string &b);
    // Normalized similarity in [0,1]; 1.0 == identical.
    static double similarity(const std::string &a, const std::string &b);
    // True when query plausibly matches cand as a typo: high relative
    // similarity, minimum query length, and no dominating prefix/suffix gap.
    static bool plausibly_matches(const std::string &query, const std::string &cand);
    // True when a prefix of cand is a plausible typo of query, ending at a
    // separator or the end of the name. This makes -fs pyhton match the whole
    // python-* family (python-pip, python-requests, ...), not just "python".
    static bool fuzzy_prefix_matches(const std::string &query, const std::string &name);
};

#endif // FUZZY_SEARCH_HPP
