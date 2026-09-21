#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <string>

enum class LogLevel
{
    INFO,
    SUCCESS,
    WARNING,
    LOG_ERROR
};

class Logger
{
public:
    static void init(const std::string &log_file_path = "fpm.log");
    static void log(LogLevel level, const std::string &message);
    static void info(const std::string &message);
    static void success(const std::string &message);
    static void warn(const std::string &message);
    static void error(const std::string &message);

private:
    static std::string log_file;
    static std::string get_current_time();
};

#endif // LOGGER_HPP
