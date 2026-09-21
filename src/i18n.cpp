#include "i18n.hpp"

std::string I18n::current_locale = "en";
std::map<std::string, std::string> I18n::translations;

void I18n::set_locale(const std::string &locale)
{
    current_locale = locale;
}

std::string I18n::get(const std::string &key)
{
    std::string full_key = current_locale + "." + key;
    auto it = translations.find(full_key);
    if (it != translations.end())
        return it->second;

    auto fallback = translations.find("en." + key);
    if (fallback != translations.end())
        return fallback->second;

    return key;
}

std::string I18n::get_locale()
{
    return current_locale;
}
