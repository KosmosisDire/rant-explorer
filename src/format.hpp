/* How numbers read in the UI. A negative value means not observable yet and reads as a
   dash, what the C explorer spells ND_DASH. */
#ifndef FORMAT_HPP
#define FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

extern const char* const DASH;

std::string format_duration(double seconds);   /* "9s", "3m 24s", "4h 12m" */
std::string format_age(double seconds);        /* "0.4s ago", "12s ago", "3m ago" */
std::string format_bytes(double bytes);        /* "512 B", "12 KB", "1.5 MB" */
std::string format_ms(double ms);              /* "0.42 ms", "120 ms" */
std::string format_number(double value);       /* thousands separated: "12,345" */
std::string format_rate(double hz);             /* "0.5 Hz", "20 Hz", "1.2k Hz" */

/* A Timestamp, microseconds since the Unix epoch in UTC, as the UI reads it to the
   millisecond, a dash for none, and as ISO 8601 to the microsecond, empty for none. */
std::string format_timestamp(int64_t us);       /* "2026-09-24 19:56:41.123" */
std::string format_iso_time(int64_t us);        /* "2026-09-24T19:56:41.123456Z" */
std::string format_span(int64_t us);            /* a Duration: "1 d 2 h 3 m 4 s" */
std::string format_clock(uint64_t unix_us);     /* local time of day: "15:56:41" */
std::string format_hex(const uint8_t* data, size_t size);   /* "00ff1a" */

#endif /* FORMAT_HPP */
