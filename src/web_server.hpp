/* rant_explorer --web-server: the web build, embedded in this exe, served over HTTP so a
   browser needs nothing else to open the explorer. */
#ifndef WEB_SERVER_HPP
#define WEB_SERVER_HPP

#include <string>

struct WebServerOptions {
    int         port = 8090;
    std::string bind = "0.0.0.0";
    std::string log_file;          /* empty: the terminal that started the exe, if any */
};

/* Serves until Ctrl-C, SIGTERM or rant web stop and returns the exit code. While it listens,
   explorer-web.state in the Rant home says where, for rant web status. */
int web_server(const WebServerOptions& options);

#endif /* WEB_SERVER_HPP */
