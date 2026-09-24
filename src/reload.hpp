/* Watches the assets directory so an edited RML or RCSS shows up without a rebuild. */
#ifndef RELOAD_HPP
#define RELOAD_HPP

#include <cstdint>
#include <map>
#include <string>

class AssetWatcher {
public:
    explicit AssetWatcher(std::string directory);

    /* True once for each batch of edits. Cheap to call every frame: it only stats the
       directory a few times a second. */
    bool changed();

private:
    void scan(std::map<std::string, int64_t>& out) const;

    std::string                    directory_;
    std::map<std::string, int64_t> stamps_;
    uint64_t                       last_scan_us_ = 0;
};

#endif /* RELOAD_HPP */
