/* Each embedded file is stored gzipped and goes out as it is to a browser that takes gzip,
   which every browser does. */

#include <httplib.h>   /* first, since on Windows it brings winsock2.h before windows.h */

#include "web_server.hpp"

#include <stb_image.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <thread>

extern const char* const          web_names[];
extern const unsigned char* const web_data[];
extern const size_t               web_sizes[];
extern const size_t               web_count;

namespace {

std::atomic<bool> g_stop{ false };

bool stop_requested() { return g_stop.load(); }

#ifdef _WIN32
BOOL WINAPI on_console(DWORD) { g_stop = true; return TRUE; }

LRESULT CALLBACK on_window(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        g_stop = true;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* rant stop, as taskkill does, asks a program with no console to close its windows, so a
   hidden one stands for the server. */
void close_on_window()
{
    std::thread([] {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = on_window;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"rant-explorer-web";
        RegisterClassW(&wc);
        if (!CreateWindowExW(0, wc.lpszClassName, L"rant-explorer-web", 0, 0, 0, 0, 0, nullptr, nullptr,
                             wc.hInstance, nullptr))
            return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    }).detach();
}
#else
void on_signal(int) { g_stop = true; }
#endif

struct File {
    const unsigned char* gz;
    size_t               size;
    const char*          type;
    std::string          etag;
};

bool ends_with(const std::string& s, const char* tail)
{
    size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

const char* content_type(const std::string& name)
{
    static const std::pair<const char*, const char*> types[] = {
        { ".html", "text/html; charset=utf-8" },  { ".js", "text/javascript; charset=utf-8" },
        { ".wasm", "application/wasm" },          { ".png", "image/png" },
        { ".json", "application/json" },         { ".css", "text/css; charset=utf-8" },
        { ".svg", "image/svg+xml" },              { ".ico", "image/x-icon" } };
    for (const auto& t : types)
        if (ends_with(name, t.first)) return t.second;
    return "application/octet-stream";
}

/* Weak, since the gzipped and the plain bodies of a file share it. */
std::string etag(const unsigned char* data, size_t size)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211ull;
    char out[32];
    std::snprintf(out, sizeof out, "W/\"%016llx\"", (unsigned long long)h);
    return out;
}

/* The embed writes a bare 10 byte gzip head and an 8 byte tail around the deflate stream. */
std::optional<std::string> gunzip(const unsigned char* gz, size_t size)
{
    if (size < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[3] != 0) return std::nullopt;
    int len = 0;
    char* raw = stbi_zlib_decode_noheader_malloc((const char*)gz + 10, (int)(size - 18), &len);
    if (!raw) return std::nullopt;
    std::string out(raw, (size_t)len);
    stbi_image_free(raw);
    return out;
}

void serve(const std::map<std::string, File>& files, const httplib::Request& req, httplib::Response& res)
{
    std::string name = req.path.substr(1);
    if (name.empty() || name.back() == '/') name += "index.html";
    auto it = files.find(name);
    if (it == files.end()) {
        res.status = 404;
        res.set_content("not found\n", "text/plain; charset=utf-8");
        return;
    }
    const File& file = it->second;
    res.set_header("ETag", file.etag);
    res.set_header("Cache-Control", "no-cache");   /* asked again each load, so a new exe shows at once */
    res.set_header("Vary", "Accept-Encoding");
    if (req.get_header_value("If-None-Match").find(file.etag) != std::string::npos) {
        res.status = 304;
        return;
    }
    if (req.get_header_value("Accept-Encoding").find("gzip") != std::string::npos) {
        res.set_header("Content-Encoding", "gzip");
        res.set_content(reinterpret_cast<const char*>(file.gz), file.size, file.type);
        return;
    }
    std::optional<std::string> body = gunzip(file.gz, file.size);
    if (!body) {
        res.status = 500;
        res.set_content("cannot inflate " + name + "\n", "text/plain; charset=utf-8");
        return;
    }
    res.set_content(std::move(*body), file.type);
}

} /* namespace */

int web_server(const WebServerOptions& options)
{
    if (web_count == 0) {
        std::fprintf(stderr, "rant_explorer: this build holds no web page, configure it with "
                             "-DEXPLORER_WEB_DIR set to a web build's bin/web\n");
        return 1;
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console, TRUE);
    close_on_window();
#else
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
#endif

    std::map<std::string, File> files;
    for (size_t i = 0; i < web_count; i++)
        files[web_names[i]] = File{ web_data[i], web_sizes[i], content_type(web_names[i]),
                                    etag(web_data[i], web_sizes[i]) };

    httplib::Server server;
    /* Not httplib's default, under which a second server shares the port without a word. */
    server.set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    server.Get(".*", [&files](const httplib::Request& req, httplib::Response& res) { serve(files, req, res); });
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::fprintf(stderr, "%s %s %d %s\n", req.method.c_str(), req.path.c_str(), res.status,
                     req.remote_addr.c_str());
    });
    if (!server.bind_to_port(options.bind, options.port)) {
        std::fprintf(stderr, "rant_explorer: cannot listen on %s:%d\n", options.bind.c_str(), options.port);
        return 1;
    }

    std::string host = options.bind == "0.0.0.0" ? "127.0.0.1" : options.bind;
    std::string url = "http://" + host + ":" + std::to_string(options.port);
    std::fprintf(stderr, "rant_explorer %s serving the web explorer at %s\n", RANT_EXPLORER_VERSION, url.c_str());

    std::thread watch([&server] {
        while (!stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
    });
    server.listen_after_bind();
    bool stopped = stop_requested();
    g_stop = true;
    watch.join();
    if (!stopped) std::fprintf(stderr, "rant_explorer: the server stopped by itself\n");
    return stopped ? 0 : 1;
}
