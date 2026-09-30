#include "src/Render/capture_file.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

// See capture_file.h. Host byte order, same machine only.

namespace band3::render {
namespace {

constexpr char kMagic[8] = {'B', '3', 'C', 'A', 'P', '0', '0', '1'};
constexpr uint32_t kMaxSavedTexture = 512;

struct Writer {
    std::vector<uint8_t> out;
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    template <typename T>
    void Put(const T& v) {
        Raw(&v, sizeof(T));
    }
};

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;
    bool ok = true;
    void Raw(void* p, size_t n) {
        if (pos + n > in.size()) {
            ok = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, in.data() + pos, n);
        pos += n;
    }
    template <typename T>
    T Get() {
        T v{};
        Raw(&v, sizeof(T));
        return v;
    }
};

Texture Downsample(const Texture& t) {
    uint32_t step = 1;
    while (t.width / step > kMaxSavedTexture || t.height / step > kMaxSavedTexture) step *= 2;
    if (step == 1) return t;
    Texture r;
    r.format = t.format;
    r.width = std::max(1u, t.width / step);
    r.height = std::max(1u, t.height / step);
    r.rgba.resize(size_t(r.width) * r.height);
    for (uint32_t y = 0; y < r.height; y++)
        for (uint32_t x = 0; x < r.width; x++)
            r.rgba[size_t(y) * r.width + x] = t.rgba[size_t(y * step) * t.width + x * step];
    return r;
}

}  // namespace

bool SaveCapture(const std::string& path, const FrameCapture& fc) {
    std::unordered_map<const Geometry*, uint32_t> geoms;
    std::unordered_map<const Texture*, uint32_t> texs;
    std::vector<const Geometry*> geom_list;
    std::vector<const Texture*> tex_list;
    for (const DrawItem& d : fc.draws) {
        if (geoms.emplace(d.geom.get(), uint32_t(geom_list.size())).second)
            geom_list.push_back(d.geom.get());
        if (d.tex && texs.emplace(d.tex.get(), uint32_t(tex_list.size())).second)
            tex_list.push_back(d.tex.get());
    }

    Writer w;
    w.Raw(kMagic, sizeof(kMagic));
    w.Put<uint64_t>(fc.frame);
    w.Put<uint32_t>(uint32_t(geom_list.size()));
    for (const Geometry* g : geom_list) {
        w.Put<uint32_t>(uint32_t(g->verts.size()));
        w.Raw(g->verts.data(), g->verts.size() * sizeof(Vertex));
        w.Put<uint32_t>(uint32_t(g->indices.size()));
        w.Raw(g->indices.data(), g->indices.size() * sizeof(uint16_t));
    }
    w.Put<uint32_t>(uint32_t(tex_list.size()));
    for (const Texture* t : tex_list) {
        const Texture s = Downsample(*t);
        w.Put<uint32_t>(s.width);
        w.Put<uint32_t>(s.height);
        w.Put<uint32_t>(s.format);
        w.Raw(s.rgba.data(), s.rgba.size() * sizeof(uint32_t));
    }
    w.Put<uint32_t>(uint32_t(fc.draws.size()));
    for (const DrawItem& d : fc.draws) {
        w.Put<uint32_t>(geoms[d.geom.get()]);
        w.Put<int32_t>(d.tex ? int32_t(texs[d.tex.get()]) : -1);
        w.Put(d.world);
        w.Put(d.view_proj);
        w.Put<uint32_t>(uint32_t(d.bones.size()));
        w.Raw(d.bones.data(), d.bones.size() * sizeof(Mat4));
        w.Raw(d.color, sizeof(d.color));
        w.Put<int32_t>(d.blend);
        w.Put<int32_t>(d.z_mode);
        w.Put<uint8_t>(d.prelit);
        w.Put<uint8_t>(d.alpha_cut);
        w.Put<int32_t>(d.alpha_threshold);
        w.Put<uint32_t>(d.cam);
        w.Put<uint32_t>(d.mesh);
    }

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fwrite(w.out.data(), 1, w.out.size(), f);
    std::fclose(f);
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::shared_ptr<FrameCapture> LoadCapture(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::vector<uint8_t> data;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);

    Reader r{data};
    char magic[8];
    r.Raw(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0) return nullptr;
    auto fc = std::make_shared<FrameCapture>();
    fc->frame = r.Get<uint64_t>();
    std::vector<std::shared_ptr<const Geometry>> geoms(r.Get<uint32_t>());
    for (auto& g : geoms) {
        auto geom = std::make_shared<Geometry>();
        geom->verts.resize(r.Get<uint32_t>());
        r.Raw(geom->verts.data(), geom->verts.size() * sizeof(Vertex));
        geom->indices.resize(r.Get<uint32_t>());
        r.Raw(geom->indices.data(), geom->indices.size() * sizeof(uint16_t));
        g = std::move(geom);
    }
    std::vector<std::shared_ptr<const Texture>> texs(r.Get<uint32_t>());
    for (auto& t : texs) {
        auto tex = std::make_shared<Texture>();
        tex->width = r.Get<uint32_t>();
        tex->height = r.Get<uint32_t>();
        tex->format = r.Get<uint32_t>();
        tex->rgba.resize(size_t(tex->width) * tex->height);
        r.Raw(tex->rgba.data(), tex->rgba.size() * sizeof(uint32_t));
        t = std::move(tex);
    }
    fc->draws.resize(r.Get<uint32_t>());
    for (DrawItem& d : fc->draws) {
        const uint32_t gi = r.Get<uint32_t>();
        const int32_t ti = r.Get<int32_t>();
        if (gi >= geoms.size() || ti >= int32_t(texs.size())) return nullptr;
        d.geom = geoms[gi];
        if (ti >= 0) d.tex = texs[ti];
        d.world = r.Get<Mat4>();
        d.view_proj = r.Get<Mat4>();
        d.bones.resize(r.Get<uint32_t>());
        r.Raw(d.bones.data(), d.bones.size() * sizeof(Mat4));
        r.Raw(d.color, sizeof(d.color));
        d.blend = r.Get<int32_t>();
        d.z_mode = r.Get<int32_t>();
        d.prelit = r.Get<uint8_t>() != 0;
        d.alpha_cut = r.Get<uint8_t>() != 0;
        d.alpha_threshold = r.Get<int32_t>();
        d.cam = r.Get<uint32_t>();
        d.mesh = r.Get<uint32_t>();
    }
    return r.ok ? fc : nullptr;
}

}  // namespace band3::render
