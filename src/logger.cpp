#include "../include/logger.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <sstream>

std::string Logger::log_file = "fpm.log";

const std::string COLOR_RESET = "\033[0m";
const std::string COLOR_CYAN = "\033[36m";
const std::string COLOR_GREEN = "\033[32m";
const std::string COLOR_YELLOW = "\033[33m";
const std::string COLOR_RED = "\033[31m";
const std::string COLOR_BOLD = "\033[1m";

std::string Logger::get_current_time()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void Logger::init(const std::string &log_file_path)
{
    log_file = log_file_path;
}

void Logger::log(LogLevel level, const std::string &message)
{
    std::string timestamp = get_current_time();
    std::string level_str;
    std::string color_code;

    switch (level)
    {
    case LogLevel::INFO:
        level_str = "INFO";
        color_code = COLOR_CYAN;
        break;
    case LogLevel::SUCCESS:
        level_str = "SUCCESS";
        color_code = COLOR_GREEN;
        break;
    case LogLevel::WARNING:
        level_str = "WARN";
        color_code = COLOR_YELLOW;
        break;
    case LogLevel::LOG_ERROR:
        level_str = "ERROR";
        color_code = COLOR_RED;
        break;
    }

    // Вывод в консоль
    std::cout << COLOR_BOLD << "[" << timestamp << "] " << color_code << "[" << level_str << "]"
              << COLOR_RESET << " " << message << std::endl;

    // Запись в файл
    std::ofstream file(log_file, std::ios::app);
    if (file.is_open())
    {
        file << "[" << timestamp << "] [" << level_str << "] " << message << "\n";
    }
}

void Logger::info(const std::string &message)
{
    log(LogLevel::INFO, message);
}

void Logger::success(const std::string &message)
{
    log(LogLevel::SUCCESS, message);
}

void Logger::warn(const std::string &message)
{
    log(LogLevel::WARNING, message);
}

void Logger::error(const std::string &message)
{
    log(LogLevel::LOG_ERROR, message);
}
