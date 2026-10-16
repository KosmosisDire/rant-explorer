#include "output.hpp"

#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

void route_output(const std::string& log_file)
{
    if (!log_file.empty()) {
        fs::path p(log_file);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && fs::file_size(p, ec) > (10u << 20)) fs::rename(p, fs::path(log_file + ".1"), ec);
        std::freopen(log_file.c_str(), "a", stdout);
        std::freopen(log_file.c_str(), "a", stderr);
    }
#ifdef _WIN32
    else if (GetStdHandle(STD_ERROR_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}
