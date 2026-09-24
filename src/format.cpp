#include "format.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>

const char* const DASH = "\xE2\x80\x94";

namespace {

std::string print(const char* fmt, ...)
{
    char buf[64];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

/* The UTC calendar time of us and the microseconds past its second, which stay positive
   before the epoch. False when the time has no calendar date. */
bool utc(int64_t us, std::tm& out, int64_t& frac)
{
    int64_t sec = us / 1000000;
    frac = us % 1000000;
    if (frac < 0) {
        frac += 1000000;
        sec--;
    }
    const std::time_t t = (std::time_t)sec;
    const std::tm* at = std::gmtime(&t);
    if (!at) return false;
    out = *at;
    return true;
}

} /* namespace */

std::string format_duration(double seconds)
{
    if (seconds < 0) return DASH;
    if (seconds < 60)   return print("%ds", (int)(seconds + 0.5));
    if (seconds < 3600) return print("%dm %02ds", (int)seconds / 60, (int)seconds % 60);
    return print("%dh %02dm", (int)seconds / 3600, ((int)seconds % 3600) / 60);
}

std::string format_age(double seconds)
{
    if (seconds < 0) return DASH;
    if (seconds < 10)   return print("%.1fs ago", seconds);
    if (seconds < 60)   return print("%ds ago", (int)seconds);
    if (seconds < 3600) return print("%dm ago", (int)seconds / 60);
    return print("%dh ago", (int)seconds / 3600);
}

std::string format_bytes(double bytes)
{
    if (bytes < 0) return DASH;
    if (bytes < 1024)                 return print("%.0f B", bytes);
    if (bytes < 1024.0 * 1024)        return print("%.0f KB", bytes / 1024.0);
    if (bytes < 1024.0 * 1024 * 1024) return print("%.1f MB", bytes / (1024.0 * 1024));
    return print("%.2f GB", bytes / (1024.0 * 1024 * 1024));
}

std::string format_ms(double ms)
{
    if (ms < 0) return DASH;
    return ms >= 100.0 ? print("%.0f ms", ms) : print("%.2f ms", ms);
}

std::string format_number(double value)
{
    if (value < 0) return DASH;
    const std::string plain = print("%.0f", value);
    std::string out;
    const size_t len = plain.size();
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) out += ',';
        out += plain[i];
    }
    return out;
}

std::string format_rate(double hz)
{
    if (hz < 0)    return DASH;
    if (hz < 10)   return print("%.1f Hz", hz);
    if (hz < 1000) return print("%.0f Hz", hz);
    return print("%.1fk Hz", hz / 1000);
}

std::string format_timestamp(int64_t us)
{
    std::tm t;
    int64_t frac = 0;
    if (!utc(us, t, frac)) return DASH;
    return print("%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                 t.tm_min, t.tm_sec, (int)(frac / 1000));
}

std::string format_iso_time(int64_t us)
{
    std::tm t;
    int64_t frac = 0;
    if (!utc(us, t, frac)) return {};
    return print("%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                 t.tm_min, t.tm_sec, (long long)frac);
}

std::string format_span(int64_t us)
{
    const bool negative = us < 0;
    int64_t sec = (negative ? -us : us) / 1000000;
    const int64_t days = sec / 86400;
    sec %= 86400;
    const std::string day = days ? std::to_string(days) + " d " : std::string();
    return print("%s%s%lld h %lld m %lld s", negative ? "-" : "", day.c_str(), (long long)(sec / 3600),
                 (long long)(sec % 3600 / 60), (long long)(sec % 60));
}

std::string format_clock(uint64_t unix_us)
{
    const std::time_t t = (std::time_t)(unix_us / 1000000u);
    const std::tm* local = unix_us ? std::localtime(&t) : nullptr;
    if (!local) return "--:--:--";
    return print("%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
}

std::string format_hex(const uint8_t* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; i++) {
        out[i * 2]     = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    return out;
}
