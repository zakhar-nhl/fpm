#include "fuzzy_search.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>

int FuzzySearch::distance(const std::string &a, const std::string &b)
{
    size_t m = a.size();
    size_t n = b.size();

    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    // Damerau-Levenshtein: adjacent transpositions cost a single edit, so
    // real-life typos like "pyhton"->"python" are distance 1 instead of 2.
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));

    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i)
    {
        for (size_t j = 1; j <= n; ++j)
        {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i - 1][j] + 1,
                dp[i][j - 1] + 1,
                dp[i - 1][j - 1] + cost,
            });

            if (i > 1 && j > 1 &&
                a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
            {
                dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1);
            }
        }
    }

    return dp[m][n];
}

double FuzzySearch::similarity(const std::string &a, const std::string &b)
{
    size_t max_len = std::max(a.size(), b.size());
    if (max_len == 0)
        return 1.0;
    return 1.0 - static_cast<double>(distance(a, b)) / static_cast<double>(max_len);
}

bool FuzzySearch::plausibly_matches(const std::string &query, const std::string &cand)
{
    if (query.empty() || cand.empty())
        return false;

    // Very short queries match too many unrelated names; exact/prefix/
    // substring tiers handle them. Fuzzy only helps real multi-char typos.
    if (query.size() < 3)
        return false;

    // A candidate more than twice as long as the query is a different word,
    // not a typo of the query. This keeps "pyhton" away from long unrelated
    // python-* bindings* unless they actually start with the typo itself.
    if (cand.size() > query.size() * 2)
        return false;

    const size_t max_len = std::max(query.size(), cand.size());
    const int d = distance(query, cand);

    // Hard edit budget: at most 3 edits, and never more than half the length
    // of the longer string (an extra conservative cap on absurd matches).
    if (d > 3 || d * 2 > static_cast<int>(max_len))
        return false;

    // Relative similarity gate. "pyhton" vs "python" scores 0.833 and passes;
    // "pyhton" vs "htop" (0.17) or "php" (0.17) never reaches the bar.
    if (similarity(query, cand) < 0.6)
        return false;

    // The strings must also share the leading letters when neither is a pure
    // transposition typo of the other. Two common leading characters keep
    // "pyhton"->"python" alive while rejecting near-miss families that only
    // share the first letter ("pyhton" vs "proton-vpn-*").
    size_t common = 0;
    size_t lim = std::min(query.size(), cand.size());
    while (common < lim &&
           std::tolower(static_cast<unsigned char>(query[common])) ==
               std::tolower(static_cast<unsigned char>(cand[common])))
        ++common;
    if (common < 2)
    {
        // Only a clean adjacent transposition can rescue short overlaps that
        // fail the two-leading-letter gate ("ahce"->"ache" shares just "a").
        size_t i = 0, j = 0;
        bool saw_swap = false;
        while (i < query.size() && j < cand.size())
        {
            if (query[i] == cand[j])
            {
                ++i;
                ++j;
                continue;
            }
            if (!saw_swap && i + 1 < query.size() && j + 1 < cand.size() &&
                query[i] == cand[j + 1] && query[i + 1] == cand[j])
            {
                saw_swap = true;
                i += 2;
                j += 2;
                continue;
            }
            if (!saw_swap)
                return false;
            ++i;
            ++j;
        }
        return saw_swap;
    }

    return true;
}

bool FuzzySearch::fuzzy_prefix_matches(const std::string &query, const std::string &name)
{
    if (query.size() < 3)
        return false;
    if (name.size() < query.size())
        return false;

    std::string prefix = name.substr(0, query.size());
    for (char &c : prefix)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // The leading core must be a plausible typo of the query...
    if (!plausibly_matches(query, prefix))
        return false;

    // ...and it must end at a boundary: the end of the name or a separator.
    // "pyhton" matches "python-pip"/"python_requests"/"python.lua" but not
    // "pyhtonscript" or unrelated long names that merely share a first letter.
    if (name.size() == query.size())
        return true;
    char next = name[query.size()];
    return !std::isalnum(static_cast<unsigned char>(next));
}

std::vector<SearchResult> FuzzySearch::search(const std::string &query,
                                              const std::vector<std::string> &candidates,
                                              int max_results)
{
    std::vector<SearchResult> results;

    for (const auto &cand : candidates)
    {
        if (!plausibly_matches(query, cand))
            continue;

        double sim = similarity(query, cand);

        int prefix_bonus = 0;
        size_t common = 0;
        size_t lim = std::min(query.size(), cand.size());
        while (common < lim &&
               std::tolower(static_cast<unsigned char>(query[common])) ==
                   std::tolower(static_cast<unsigned char>(cand[common])))
            ++common;
        prefix_bonus = static_cast<int>(common);

        int score = static_cast<int>(std::round(100.0 * sim)) + prefix_bonus;

        results.push_back({cand, score});
    }

    std::sort(results.begin(), results.end(),
              [](const SearchResult &a, const SearchResult &b)
              {
                  if (a.score != b.score)
                      return a.score > b.score;
                  if (a.name.size() != b.name.size())
                      return a.name.size() < b.name.size();
                  return a.name < b.name;
              });

    if (static_cast<int>(results.size()) > max_results)
        results.resize(max_results);

    return results;
}