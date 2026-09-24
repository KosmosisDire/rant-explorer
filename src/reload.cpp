#include "reload.hpp"

#include <chrono>
#include <filesystem>

namespace {

uint64_t steady_us()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

/* An editor often writes a file in more than one step, so a scan this soon after the last
   change would catch it half written. */
constexpr uint64_t SCAN_PERIOD_US = 250000;

} /* namespace */

AssetWatcher::AssetWatcher(std::string directory) : directory_(std::move(directory))
{
    scan(stamps_);
    last_scan_us_ = steady_us();
}

void AssetWatcher::scan(std::map<std::string, int64_t>& out) const
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto stamp = fs::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        out[it->path().filename().string()] = stamp.time_since_epoch().count();
    }
}

bool AssetWatcher::changed()
{
    const uint64_t now = steady_us();
    if (now - last_scan_us_ < SCAN_PERIOD_US) return false;
    last_scan_us_ = now;

    std::map<std::string, int64_t> fresh;
    scan(fresh);
    if (fresh.empty() || fresh == stamps_) return false;

    stamps_ = std::move(fresh);
    return true;
}
