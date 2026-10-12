#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include "src/Net/http_request.h"
#include "src/Net/milo.h"

// The web server's /game_asset replies (http_server.h) that can be worked out
// without the game: what's kept, and /game_asset/milo_texture's. The server
// hands in its way of reading a game file on the game thread.

namespace band3::http {

// a /game_asset reply: a PNG, a font's JSON, or a 404
struct Asset {
    int status = 404;
    std::string type = "text/plain; charset=utf-8";
    std::string body = "No such game asset";
};

// replies by key ("t:" a texture's path, "f:"/"p:" a font's name, "m:" a milo
// and a texture's name), the newest `max` kept
class AssetCache {
public:
    explicit AssetCache(size_t max) : max_(max) {}
    std::optional<Asset> Get(const std::string& key);
    void Put(const std::string& key, const Asset& asset);

private:
    size_t max_;
    std::mutex mutex_;
    std::deque<std::pair<std::string, Asset>> kept_;  // newest last
};

// a game file read on the game thread: busy when the game couldn't take the
// read yet; otherwise the file, or nullopt when it isn't there
struct GameRead {
    bool busy = false;
    std::optional<std::string> file;
};
using ReadGameFile = std::function<GameRead(const std::string& path)>;

// /game_asset/milo_texture's reply (http_request.h's kGameMiloTexture): the
// texture as a PNG, or a 404 for a milo or name the allowlists refuse, a milo
// that isn't there or doesn't decompress, or a texture it doesn't have (or in
// a format band3 doesn't decode). nullopt while the game is busy, kept for
// nothing. The milos read are kept in `milos`, and the replies in `assets`.
std::optional<Asset> MiloTexture(const Route& route, AssetCache& assets, milo::MiloCache& milos,
                                 const ReadGameFile& read);

}
