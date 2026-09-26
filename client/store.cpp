#include "store.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "edition.h"
#include "rd_crypto.h"

namespace td {

namespace {

// Records are tab-separated lines; escape the few characters that matter.
std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

std::string unesc(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') out.push_back(unesc(cur)), cur.clear();
        else cur += c;
    }
    out.push_back(unesc(cur));
    return out;
}

std::string key_for(const std::string &host, int port) { return host + ":" + std::to_string(port); }

}  // namespace

void Store::load() {
#ifndef __EMSCRIPTEN__
    // TETHERDESK_DATA_DIR lets tests and demos use a separate data folder.
    if (const char *override_dir = std::getenv("TETHERDESK_DATA_DIR"); override_dir && *override_dir) {
        dir_ = override_dir;
        if (dir_.back() != '/' && dir_.back() != '\\') dir_ += '/';
    } else if (char *p = SDL_GetPrefPath("TetherDesk", edition::data_folder)) {
        dir_ = p;
        SDL_free(p);
    }
#endif
    if (dir_.empty()) return;
    std::ifstream f(dir_ + "viewer.tsv");
    std::string line;
    while (std::getline(f, line)) {
        auto v = split_tabs(line);
        if (v.size() >= 10 && v[0] == "pc") {
            SavedPc pc;
            pc.id = v[1];
            pc.label = v[2];
            pc.host = v[3];
            pc.port = std::atoi(v[4].c_str());
            pc.user_name = v[5];
            pc.remember = v[6] == "1";
            pc.fullscreen = v[7] == "1";
            pc.last_used = std::atoll(v[8].c_str());
            pc.password = pc.remember ? v[9] : "";
            if (!pc.id.empty() && !pc.host.empty()) pcs.push_back(pc);
        } else if (v.size() >= 4 && v[0] == "share") {
            share_password = v[1];
            share_view_only = v[2] == "1";
            share_demo = v[3] == "1";
            share_always_on = v.size() >= 5 && v[4] == "1";
        } else if (v.size() >= 3 && v[0] == "known") {
            known_[v[1]] = v[2];
        }
    }
}

void Store::save() const {
    if (dir_.empty()) return;
    const std::string path = dir_ + "viewer.tsv", tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        for (const SavedPc &pc : pcs)
            f << "pc\t" << esc(pc.id) << '\t' << esc(pc.label) << '\t' << esc(pc.host) << '\t' << pc.port << '\t'
              << esc(pc.user_name) << '\t' << (pc.remember ? 1 : 0) << '\t' << (pc.fullscreen ? 1 : 0) << '\t'
              << pc.last_used << '\t' << esc(pc.remember ? pc.password : "") << '\n';
        for (const auto &k : known_) f << "known\t" << esc(k.first) << '\t' << esc(k.second) << '\n';
        f << "share\t" << esc(share_password) << '\t' << (share_view_only ? 1 : 0) << '\t' << (share_demo ? 1 : 0)
          << '\t' << (share_always_on ? 1 : 0) << '\n';
    }
#ifndef _WIN32
    chmod(tmp.c_str(), 0600);  // may contain remembered passwords
#endif
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

SavedPc *Store::find(const std::string &id) {
    for (auto &pc : pcs)
        if (pc.id == id) return &pc;
    return nullptr;
}

void Store::upsert(const SavedPc &pc) {
    if (SavedPc *e = find(pc.id)) *e = pc;
    else pcs.push_back(pc);
}

void Store::remove(const std::string &id) {
    pcs.erase(std::remove_if(pcs.begin(), pcs.end(), [&](const SavedPc &p) { return p.id == id; }), pcs.end());
    if (!dir_.empty()) std::remove(thumbnail_path(id).c_str());
}

std::string Store::new_id() const {
    uint8_t r[6];
    rd_random(r, sizeof r);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
    return buf;
}

std::string Store::known_fingerprint(const std::string &host, int port) const {
    auto it = known_.find(key_for(host, port));
    return it == known_.end() ? "" : it->second;
}

void Store::trust(const std::string &host, int port, const std::string &fp) { known_[key_for(host, port)] = fp; }

std::string Store::thumbnail_path(const std::string &id) const { return dir_ + "thumb_" + id + ".bmp"; }

}  // namespace td
