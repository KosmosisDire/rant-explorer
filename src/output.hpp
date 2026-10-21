/* Where stdout and stderr go. The desktop exe is windowless on Windows, so it has no console
   of its own to print to. */
#ifndef OUTPUT_HPP
#define OUTPUT_HPP

/* Where the one that started the exe sent them, rant to a node's log, else to the terminal
   that started it, if any. */
void route_output();

#endif /* OUTPUT_HPP */
