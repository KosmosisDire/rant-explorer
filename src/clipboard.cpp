#include "clipboard.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

Bytes bytes_of(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

std::string tsv(const Table& table, const char* line_end)
{
    std::string out;
    for (const std::vector<std::string>& row : table.rows) {
        for (size_t i = 0; i < row.size(); i++) out += row[i] + (i + 1 < row.size() ? "\t" : "");
        out += line_end;
    }
    return out;
}

std::string escape(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        if (c == '&')      out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else               out += c;
    }
    return out;
}

/* Bordered, since Word and PowerPoint draw a table with no border as loose text. */
std::string html(const Table& table)
{
    std::string out = "<table border=\"1\" style=\"border-collapse:collapse\">";
    for (size_t r = 0; r < table.rows.size(); r++) {
        const char* tag = table.header && r == 0 ? "th" : "td";
        out += "<tr>";
        for (const std::string& cell : table.rows[r])
            out += std::string("<") + tag + " style=\"padding:2px 6px\">" + escape(cell) + "</" + tag + ">";
        out += "</tr>";
    }
    return out + "</table>";
}

} /* namespace */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

/* SDL sets only the first text type and the first image type it is given on Windows, so
   every format goes in here, in one open of the clipboard. */
bool set_formats(SDL_Window* window, const std::vector<std::pair<UINT, Bytes>>& formats)
{
    /* A null owner makes every SetClipboardData after EmptyClipboard fail. */
    HWND owner = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!owner || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    for (const auto& format : formats) {
        HANDLE memory = GlobalAlloc(GMEM_MOVEABLE, format.second.size());
        void*  to     = memory ? GlobalLock(memory) : nullptr;
        if (!to) {
            if (memory) GlobalFree(memory);
            continue;
        }
        std::memcpy(to, format.second.data(), format.second.size());
        GlobalUnlock(memory);
        if (SetClipboardData(format.first, memory)) ok = true;
        else GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

/* UTF-16 with its terminator, what CF_UNICODETEXT holds. */
Bytes wide(const std::string& text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size() + 1, nullptr, 0);
    Bytes out((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size() + 1, (wchar_t*)out.data(), n);
    return out;
}

/* The HTML clipboard format: a header of byte offsets to the document and the fragment. */
Bytes cf_html(const std::string& fragment)
{
    const std::string before = "<html><body>\r\n<!--StartFragment-->";
    const std::string after  = "<!--EndFragment-->\r\n</body>\r\n</html>";
    const char* form = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
    const size_t head  = (size_t)std::snprintf(nullptr, 0, form, (size_t)0, (size_t)0, (size_t)0, (size_t)0);
    const size_t start = head + before.size(), end = start + fragment.size();
    std::string out(head + 1, '\0');
    std::snprintf(&out[0], out.size(), form, head, end + after.size(), start, end);
    out.resize(head);
    out += before + fragment + after;
    out += '\0';
    return bytes_of(out);
}

} /* namespace */

bool clipboard_set_image(SDL_Window* window, const Image& image)
{
    if (!image.valid()) return false;
    const Bytes bmp = image_bmp(image);
    /* A bitmap on the clipboard is the BMP file without its 14 byte file header. */
    return set_formats(window, { { (UINT)CF_DIB, Bytes(bmp.begin() + 14, bmp.end()) },
                                 { RegisterClipboardFormatA("PNG"), image_png(image) } });
}

bool clipboard_set_table(SDL_Window* window, const Table& table)
{
    if (table.rows.empty()) return false;
    return set_formats(window, { { (UINT)CF_UNICODETEXT, wide(tsv(table, "\r\n")) },
                                 { RegisterClipboardFormatA("HTML Format"), cf_html(html(table)) } });
}

#else

namespace {

/* The data behind each type, held until the clipboard is replaced, since an app asks for
   its type only when it pastes. */
struct Offer {
    std::vector<std::string> types;
    std::vector<Bytes>       data;
};

bool set_types(Offer* offer)
{
    std::vector<const char*> types;
    for (const std::string& type : offer->types) types.push_back(type.c_str());
    return SDL_SetClipboardData(
        [](void* user, const char* mime, size_t* size) -> const void* {
            const auto* o = static_cast<const Offer*>(user);
            for (size_t i = 0; i < o->types.size(); i++)
                if (o->types[i] == mime) {
                    *size = o->data[i].size();
                    return o->data[i].data();
                }
            *size = 0;
            return nullptr;
        },
        [](void* user) { delete static_cast<Offer*>(user); }, offer, types.data(), types.size());
}

} /* namespace */

bool clipboard_set_image(SDL_Window*, const Image& image)
{
    if (!image.valid()) return false;
    return set_types(new Offer{ { "image/png", "image/bmp" }, { image_png(image), image_bmp(image) } });
}

bool clipboard_set_table(SDL_Window*, const Table& table)
{
    if (table.rows.empty()) return false;
    const Bytes text = bytes_of(tsv(table, "\n"));
    return set_types(new Offer{ { "text/html", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" },
                                { bytes_of(html(table)), text, text, text, text, text } });
}

#endif
