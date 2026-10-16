/* Where stdout and stderr go. The desktop exe is windowless on Windows, so it has no console
   of its own to print to. */
#ifndef OUTPUT_HPP
#define OUTPUT_HPP

#include <string>

/* To log_file when one is given, else to the terminal that started the exe, if any. A log
   past 10 MB moves aside to log_file.1 first. */
void route_output(const std::string& log_file);

#endif /* OUTPUT_HPP */
