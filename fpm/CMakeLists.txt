cmake_minimum_required(VERSION 3.16)
project(fpm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(fpm
    src/main.cpp
    src/package.cpp
    src/logger.cpp
    src/database.cpp
)

install(TARGETS fpm DESTINATION /usr/bin)
