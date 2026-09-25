// store.h - the viewer's saved PCs, trusted host fingerprints and session
// thumbnails, kept in the per-user app data folder (SDL_GetPrefPath).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace td {

struct SavedPc {
    std::string id;
    std::string label;
    std::string host;
    int port = 5980;
    std::string user_name;
    std::string password;   // only kept when `remember` is set
    bool remember = false;
    bool fullscreen = true;  // open sessions full screen, like Remote Desktop
    int64_t last_used = 0;   // unix time
};

class Store {
public:
    // Loads from disk. Safe to call when no data exists yet.
    void load();
    void save() const;
    bool persistent() const { return !dir_.empty(); }

    std::vector<SavedPc> pcs;
    SavedPc *find(const std::string &id);
    void upsert(const SavedPc &pc);
    void remove(const std::string &id);
    std::string new_id() const;

    // Trust-on-first-use host identities, keyed by "host:port".
    std::string known_fingerprint(const std::string &host, int port) const;
    void trust(const std::string &host, int port, const std::string &fingerprint);

    std::string thumbnail_path(const std::string &id) const;
    std::string dir() const { return dir_; }

    // "Share this PC" settings.
    std::string share_password;
    bool share_view_only = false;
    bool share_demo = false;

private:
    std::string dir_;
    std::map<std::string, std::string> known_;
};

}  // namespace td
