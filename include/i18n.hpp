#ifndef I18N_HPP
#define I18N_HPP

#include <string>
#include <map>

class I18n
{
public:
    static void set_locale(const std::string &locale);
    static std::string get(const std::string &key);
    static std::string get_locale();

private:
    static std::string current_locale;
    static std::map<std::string, std::string> translations;
};

#endif // I18N_HPP
