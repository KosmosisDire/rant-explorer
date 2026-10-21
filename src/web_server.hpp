/* rant_explorer --web-server: the web build, embedded in this exe, served over HTTP so a
   browser needs nothing else to open the explorer. */
#ifndef WEB_SERVER_HPP
#define WEB_SERVER_HPP

#include <string>

struct WebServerOptions {
    int         port = 6732;     /* Rant's ports are 6730 to 6739 */
    std::string bind = "0.0.0.0";
};

/* Serves until Ctrl-C, SIGTERM or rant stop and returns the exit code. */
int web_server(const WebServerOptions& options);

#endif /* WEB_SERVER_HPP */
