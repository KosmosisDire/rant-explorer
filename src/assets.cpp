#include "assets.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

/* Written by cmake/embed.cmake from every file in assets/. */
extern const char* const          asset_names[];
extern const unsigned char* const asset_data[];
extern const size_t               asset_sizes[];
extern const size_t               asset_count;

namespace {

std::string directory;

bool read_disk(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool read_embedded(const std::string& name, std::string& out)
{
    for (size_t i = 0; i < asset_count; i++) {
        if (name != asset_names[i]) continue;
        out.assign((const char*)asset_data[i], asset_sizes[i]);
        return true;
    }
    return false;
}

struct OpenFile {
    std::string data;
    size_t      pos = 0;
};

class AssetFiles : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override
    {
        auto* file = new OpenFile;
        if (asset_read(path, file->data)) return (Rml::FileHandle)file;
        delete file;
        return 0;
    }

    void Close(Rml::FileHandle handle) override { delete (OpenFile*)handle; }

    size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override
    {
        auto* file = (OpenFile*)handle;
        size = std::min(size, file->data.size() - file->pos);
        std::memcpy(buffer, file->data.data() + file->pos, size);
        file->pos += size;
        return size;
    }

    bool Seek(Rml::FileHandle handle, long offset, int origin) override
    {
        auto* file = (OpenFile*)handle;
        const long base = origin == SEEK_SET ? 0
                        : origin == SEEK_CUR ? (long)file->pos
                                             : (long)file->data.size();
        if (base + offset < 0 || base + offset > (long)file->data.size()) return false;
        file->pos = (size_t)(base + offset);
        return true;
    }

    size_t Tell(Rml::FileHandle handle) override { return ((OpenFile*)handle)->pos; }

    size_t Length(Rml::FileHandle handle) override { return ((OpenFile*)handle)->data.size(); }

    bool LoadFile(const Rml::String& path, Rml::String& out) override { return asset_read(path, out); }
};

} /* namespace */

void assets_init()
{
    if (const char* env = std::getenv("RANT_UI_ASSETS")) {
        directory = std::string(env) + "/";
        return;
    }
#ifdef RANT_UI_SOURCE_ASSETS
    std::error_code ec;
    if (std::filesystem::is_directory(RANT_UI_SOURCE_ASSETS, ec))
        directory = std::string(RANT_UI_SOURCE_ASSETS) + "/";
#endif
}

const std::string& assets_dir() { return directory; }

bool asset_read(const std::string& name, std::string& out)
{
    if (!std::filesystem::path(name).is_relative()) return read_disk(name, out);
    if (!directory.empty()) return read_disk(directory + name, out);
    return read_embedded(name, out);
}

Rml::FileInterface& assets_file_interface()
{
    static AssetFiles files;
    return files;
}
