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
    src/slag.cpp
    src/mirror_selector.cpp
    src/ui.cpp
    src/cleaner.cpp
    src/suggest.cpp
    src/fuzzy_search.cpp
    src/json.cpp
    src/i18n.cpp
    src/repository.cpp
)

# On GCC >= 9 std::filesystem is part of libstdc++; older toolchains
# (GCC 8, some musl setups) need an explicit link against libstdc++fs.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
#include <filesystem>
int main() { std::filesystem::path p; (void)p; return 0; }
]=] FPM_FS_NEEDS_LIB)
if(NOT FPM_FS_NEEDS_LIB)
    target_link_libraries(fpm PRIVATE stdc++fs)
endif()

install(TARGETS fpm DESTINATION /usr/bin)
install(FILES etc/mirrors.list DESTINATION /etc/fpm)
