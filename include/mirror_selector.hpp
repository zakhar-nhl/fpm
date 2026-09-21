#ifndef MIRROR_SELECTOR_HPP
#define MIRROR_SELECTOR_HPP

#include <string>
#include <vector>

struct Mirror
{
    std::string url;
    std::string country;
    std::string protocol;
    int priority{};
    bool available;
    int rtt_ms{};
    std::string index;
};

class MirrorSelector
{
public:
    // Hardcoded safety-net mirror. Updates can never brick the system:
    // when every configured mirror fails, FPM always falls back to this URL.
    static const std::string &default_mirror();

    // Config file paths. mirrors.list holds the CURRENT ACTIVE mirror(s),
    // candidates.list holds the full pool of selectable mirrors.
    static std::string find_mirrors_list();       // active mirrors.list
    static std::string find_candidates_list();    // candidates.list
    static bool load_mirrors(const std::string &list_path);
    static bool load_active_mirrors();            // active file -> `mirrors`
    static bool load_candidates();                // candidates file -> `mirrors`
    static bool ensure_loaded();                  // active else candidates -> `mirrors`
    static const std::vector<Mirror> &get_mirrors();

    static void set_state_path(const std::string &file);
    static std::string get_selected();
    static void set_selected(const std::string &url);
    static const Mirror *select_best();

    // Persist <m> as the single active mirror (rewrites mirrors.list).
    static bool set_active_mirror(const Mirror &m);
    // -am: validate <url> and append it to candidates.list (or mirrors.list).
    static bool add_mirror_candidate(const std::string &url, const std::string &country,
                                     bool active);
    // -dm/-dmm: remove <url> from the candidate pool AND the active list.
    static bool remove_mirror(const std::string &url);
    // Writable config target. Repo-local etc/ and legacy /etc/mirrors.list are
    // read-only fallbacks and are never rewritten.
    static std::string write_target_mirrors();
    static std::string write_target_candidates();
    // Availability probe used by -am (host replies with an HTTP response).
    static bool is_reachable(const std::string &url, int timeout_ms = 4000);
    // Lightweight repo-format marker probe used by -ms across many mirrors.
    static bool probe_repo_marker(const std::string &base, std::string &found_type,
                                  int timeout_ms = 4000);

    static bool download(const std::string &url, const std::string &local_path, int timeout_sec = 15);
    // Bounded, no-retry fetch used for index probes (short timeouts).
    static bool download_fast(const std::string &url, const std::string &local_path);
    static int measure_latency(const std::string &url, int timeout_ms = 6000);
    static std::string fetch_text(const std::string &url, int timeout_sec = 15, size_t max_bytes = 8 * 1024 * 1024);

    // HTTP diagnostics: -1 = network/TLS error, otherwise the numeric HTTP status.
    static int http_status(const std::string &url, int timeout_ms = 5000);
    // Latency (ms) of a HEAD/GET round-trip to <url>; -1 on failure.
    static int measure_endpoint(const std::string &url, int timeout_ms = 5000);
    // Mirrors with the previously-selected mirror first, then priority order.
    static std::vector<Mirror> get_ordered_mirrors();

    static const std::vector<std::string> &index_candidates();
    static bool probe_index(const std::string &base, std::string &found_url,
                            std::string &found_type, int timeout_ms = 5000);

private:
    static std::vector<Mirror> mirrors;
    static std::string selected;
    static std::string selected_file;
    static bool parse_mirror_line(const std::string &line, Mirror &m);
    static void load_selected();
    static bool parse_into(const std::string &path, std::vector<Mirror> &out);
    static std::string active_default_path(); // writable mirrors.list location
    static std::string candidates_default_path(); // writable candidates.list location
};

#endif // MIRROR_SELECTOR_HPP