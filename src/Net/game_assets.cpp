#include "game_assets.h"
#include <memory>
#include "src/Net/album_art.h"

namespace band3::http {

std::optional<Asset> AssetCache::Get(const std::string& key) {
    std::lock_guard lock(mutex_);
    for (const auto& [k, asset] : kept_) {
        if (k == key) return asset;
    }
    return std::nullopt;
}

void AssetCache::Put(const std::string& key, const Asset& asset) {
    std::lock_guard lock(mutex_);
    kept_.emplace_back(key, asset);
    if (kept_.size() > max_) kept_.pop_front();
}

std::optional<Asset> MiloTexture(const Route& route, AssetCache& assets, milo::MiloCache& milos,
                                 const GameFileReader& read) {
    const std::string& path = route.argument;
    if (!AllowedMiloPath(path) || !AllowedTextureName(route.name)) return Asset{};
    const std::string key = "m:" + path + "|" + route.name;
    if (auto asset = assets.Get(key)) return asset;
    const milo::MiloCache::Load loaded = milos.Get(path, [&read, &path] {
        milo::MiloCache::Load out;
        const GameRead got = read(path);
        out.busy = got.busy;
        if (auto raw = got.file ? milo::Decompress(*got.file) : std::nullopt) {
            out.raw = std::make_shared<const std::string>(std::move(*raw));
        }
        return out;
    });
    if (loaded.busy) return std::nullopt;
    // another texture of it may have waited on the same load
    if (auto asset = assets.Get(key)) return asset;
    Asset asset;
    if (const auto image = loaded.raw ? milo::FindNamedBitmap(*loaded.raw, route.name) : std::nullopt) {
        asset = {200, "image/png", EncodePng(*image)};
    }
    assets.Put(key, asset);
    return asset;
}

}
