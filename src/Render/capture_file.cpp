#include "src/Render/capture_file.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

// See capture_file.h. Host byte order, same machine only.
//
// Version 3 (B3CAP003) is a list of sections, each with its own id, version
// and size, so a reader skips what it doesn't know and a struct can grow
// without every older file going unreadable:
//   FRAM  frame numbers, post-processing boundary and counts (a counted list),
//         then the world's frame (a file without it: the frame's own)
//   GEOM  geometry, with the vertex's size: Vertex only ever grows at the end,
//         so a file with another size keeps the fields both have
//   TEXS  textures, with render targets' identity; one whose pixels another
//         has already points at those
//   SHAD  shade states field by field, with the register list and the number
//         of maps they were saved with, matched by register on load
//   DRAW  draws
//   PASS  passes
// A section newer than this reader skips if it's SHAD, PASS or FRAM (the file
// loads without it) and fails the load if it's GEOM, TEXS or DRAW.
//
// Versions 1 and 2 still load: 1 is frame, geometry, textures and draws; 2
// adds the draws' ShadeStates, kept as their ShadeInputs were in memory (so
// only while that struct is the same size), and their maps among the
// textures.

namespace band3::render {
namespace {

constexpr char kMagic[8] = {'B', '3', 'C', 'A', 'P', '0', '0', '3'};
constexpr char kMagicV2[8] = {'B', '3', 'C', 'A', 'P', '0', '0', '2'};
constexpr char kMagicV1[8] = {'B', '3', 'C', 'A', 'P', '0', '0', '1'};
constexpr uint32_t kMaxSavedTexture = 512;
// a ShadeState's maps are kept smaller: at 512 they made a song's captures
// half again as big
constexpr uint32_t kMaxSavedMap = 256;

constexpr uint32_t FourCC(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}
constexpr uint32_t kSecFrame = FourCC("FRAM");
constexpr uint32_t kSecGeometry = FourCC("GEOM");
constexpr uint32_t kSecTextures = FourCC("TEXS");
constexpr uint32_t kSecShades = FourCC("SHAD");
constexpr uint32_t kSecDraws = FourCC("DRAW");
constexpr uint32_t kSecPasses = FourCC("PASS");
// the versions this build writes and reads
constexpr uint32_t kFrameVersion = 1;
constexpr uint32_t kGeometryVersion = 1;
constexpr uint32_t kTexturesVersion = 1;
constexpr uint32_t kShadesVersion = 1;
constexpr uint32_t kDrawsVersion = 1;
constexpr uint32_t kPassesVersion = 1;

// TEXS: where a texture's pixels are
constexpr int32_t kOwnPixels = -1;  // they follow
constexpr int32_t kNoPixels = -2;   // none (a render target kept by identity alone)

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
    // a section's header, its size filled in by End
    size_t Begin(uint32_t id, uint32_t version) {
        Put<uint32_t>(id);
        Put<uint32_t>(version);
        Put<uint64_t>(0);
        return out.size();
    }
    void End(size_t start) {
        const uint64_t size = out.size() - start;
        std::memcpy(out.data() + start - sizeof(uint64_t), &size, sizeof(size));
    }
};

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;
    size_t end = 0;  // of the section being read, or of the data
    bool ok = true;
    void Raw(void* p, size_t n) {
        if (pos + n > end) {
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
    // a count of items at least `each` bytes, false if the rest can't hold them
    bool Count(uint32_t& n, size_t each) {
        n = Get<uint32_t>();
        if (!ok || (each && uint64_t(n) * each > end - pos)) ok = false;
        return ok;
    }
};

Texture Downsample(const Texture& t, uint32_t most) {
    uint32_t step = 1;
    while (t.width / step > most || t.height / step > most) step *= 2;
    if (step == 1) return t;
    Texture r;
    r.format = t.format;
    r.tex_obj = t.tex_obj;
    r.tex_type = t.tex_type;
    r.version = t.version;
    r.width = std::max(1u, t.width / step);
    r.height = std::max(1u, t.height / step);
    r.rgba.resize(size_t(r.width) * r.height);
    for (uint32_t y = 0; y < r.height; y++)
        for (uint32_t x = 0; x < r.width; x++)
            r.rgba[size_t(y) * r.width + x] = t.rgba[size_t(y * step) * t.width + x * step];
    return r;
}

uint64_t PixelHash(const Texture& t) {
    uint64_t h = 1469598103934665603ull ^ t.width ^ (uint64_t(t.height) << 20) ^
                 (uint64_t(t.format) << 40);
    for (uint32_t p : t.rgba) h = (h ^ p) * 1099511628211ull;
    return h;
}

bool SameIdentity(const Texture& a, const Texture& b) {
    return a.tex_obj == b.tex_obj && a.tex_type == b.tex_type && a.version == b.version;
}

bool SamePixels(const Texture& a, const Texture& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.rgba == b.rgba;
}

// ShadeInputs field by field, its registers and maps as the section says
void PutShade(Writer& w, const ShadeInputs& s) {
    w.Put(s.options);
    w.Put(s.shader_type);
    w.Put(s.env);
    w.Raw(s.eye, sizeof(s.eye));
    w.Raw(s.vs, sizeof(s.vs));
    w.Raw(s.ps, sizeof(s.ps));
    w.Put(s.mat);
    w.Put(s.next_pass);
    w.Put(s.use_environ);
    w.Put(s.intensify);
    w.Put(s.per_pixel_lit);
    w.Put(s.pad0);
    w.Put(s.shader_variation);
    w.Raw(s.mat_maps, sizeof(s.mat_maps));
    w.Raw(s.mat_map_base, sizeof(s.mat_map_base));
    w.Put(s.mat_diffuse_base);
    w.Raw(s.fetch_diffuse, sizeof(s.fetch_diffuse));
    w.Raw(s.fetch, sizeof(s.fetch));
}

// `regs`: the registers the file has, in its order; `maps`: how many maps
void GetShade(Reader& r, ShadeInputs& s, const std::vector<uint32_t>& regs, uint32_t maps) {
    std::memset(&s, 0, sizeof(s));
    s.options = r.Get<uint64_t>();
    s.shader_type = r.Get<int32_t>();
    s.env = r.Get<uint32_t>();
    r.Raw(s.eye, sizeof(s.eye));
    for (int bank = 0; bank < 2; bank++) {
        for (uint32_t reg : regs) {
            float v[4];
            r.Raw(v, sizeof(v));
            const int i = ShadeRegIndex(int(reg));
            if (i >= 0) std::memcpy(bank ? s.ps[i] : s.vs[i], v, sizeof(v));
        }
    }
    s.mat = r.Get<uint32_t>();
    s.next_pass = r.Get<uint32_t>();
    s.use_environ = r.Get<uint8_t>();
    s.intensify = r.Get<uint8_t>();
    s.per_pixel_lit = r.Get<uint8_t>();
    s.pad0 = r.Get<uint8_t>();
    s.shader_variation = r.Get<int32_t>();
    const uint32_t keep = std::min<uint32_t>(maps, kNumShadeMaps);
    auto per_map = [&](auto* out, size_t each) {
        for (uint32_t m = 0; m < maps; m++) {
            uint8_t buf[sizeof(uint32_t) * 6];
            r.Raw(buf, each);
            if (m < keep) std::memcpy(reinterpret_cast<uint8_t*>(out) + m * each, buf, each);
        }
    };
    per_map(s.mat_maps, sizeof(uint32_t));
    per_map(s.mat_map_base, sizeof(uint32_t));
    s.mat_diffuse_base = r.Get<uint32_t>();
    r.Raw(s.fetch_diffuse, sizeof(s.fetch_diffuse));
    per_map(s.fetch, sizeof(s.fetch[0]));
}

// FRAM's counts, in their order: only ever appended to
struct NamedCount {
    uint32_t FrameCapture::*field;
};
constexpr NamedCount kFrameCounts[] = {
    {&FrameCapture::cams},           {&FrameCapture::skipped_target},
    {&FrameCapture::skipped_velocity}, {&FrameCapture::skipped_shadow},
    {&FrameCapture::skipped_draw_mode}, {&FrameCapture::skipped_no_geom},
    {&FrameCapture::mutable_meshes}, {&FrameCapture::multimesh_instances},
    {&FrameCapture::particles},      {&FrameCapture::textured},
    {&FrameCapture::untextured_format}, {&FrameCapture::geom_cached},
    {&FrameCapture::tex_cached},     {&FrameCapture::maps_decoded},
    {&FrameCapture::maps_cube},      {&FrameCapture::maps_other_format},
    {&FrameCapture::passes_own},     {&FrameCapture::passes_carried},
    {&FrameCapture::passes_empty},   {&FrameCapture::rt_sampled},
    {&FrameCapture::rt_missing},     {&FrameCapture::rt_snapshots},
    {&FrameCapture::passes_unbalanced}, {&FrameCapture::composed},
};

// B3CAP001 and B3CAP002, after the magic
std::shared_ptr<FrameCapture> LoadOld(Reader& r, bool v1) {
    auto fc = std::make_shared<FrameCapture>();
    fc->frame = r.Get<uint64_t>();
    std::vector<std::shared_ptr<const Geometry>> geoms(r.Get<uint32_t>());
    for (auto& g : geoms) {
        auto geom = std::make_shared<Geometry>();
        geom->verts.resize(r.Get<uint32_t>());
        r.Raw(geom->verts.data(), geom->verts.size() * sizeof(Vertex));
        geom->indices.resize(r.Get<uint32_t>());
        r.Raw(geom->indices.data(), geom->indices.size() * sizeof(uint16_t));
        if (!r.ok) return nullptr;
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
        if (!r.ok) return nullptr;
        t = std::move(tex);
    }
    if (!v1) {
        // a file from a build with other registers or maps isn't read
        const uint32_t inputs_size = r.Get<uint32_t>();
        const uint32_t num_maps = r.Get<uint32_t>();
        if (inputs_size != sizeof(ShadeInputs) || num_maps != uint32_t(kNumShadeMaps))
            return nullptr;
        fc->shades.resize(r.Get<uint32_t>());
        for (ShadeState& s : fc->shades) {
            static_cast<ShadeInputs&>(s) = r.Get<ShadeInputs>();
            for (auto& m : s.maps) {
                const int32_t ti = r.Get<int32_t>();
                if (ti >= int32_t(texs.size())) return nullptr;
                if (ti >= 0) m = texs[ti];
            }
            if (!r.ok) return nullptr;
        }
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
        d.shade = v1 ? -1 : r.Get<int32_t>();
        if (d.shade >= int32_t(fc->shades.size()) || !r.ok) return nullptr;
    }
    return r.ok ? fc : nullptr;
}

}  // namespace

bool SaveCapture(const std::string& path, const FrameCapture& fc) {
    std::unordered_map<const Geometry*, uint32_t> geoms;
    std::unordered_map<const Texture*, uint32_t> texs;
    std::vector<const Geometry*> geom_list;
    struct Saved {
        const Texture* tex;
        uint32_t most;            // the largest its pixels are kept at
        int32_t pixels = kOwnPixels;  // or kNoPixels, or the entry that has them
    };
    std::vector<Saved> tex_list;
    // A texture with the same pixels as one already kept (the material's
    // colour texture as its specular map, say) is kept once, at the larger
    // size. A render target sampled at several versions keeps each, but its
    // pixels (guest memory's, which don't change between them) once.
    std::unordered_multimap<uint64_t, uint32_t> by_pixels;
    std::unordered_map<uint64_t, uint32_t> by_identity;  // pixel-less render targets
    auto add_tex = [&](const Texture* t, uint32_t most) {
        if (!t) return;
        if (auto it = texs.find(t); it != texs.end()) {
            Saved& s = tex_list[it->second];
            const uint32_t owner = s.pixels >= 0 ? uint32_t(s.pixels) : it->second;
            tex_list[owner].most = std::max(tex_list[owner].most, most);
            return;
        }
        const uint32_t index = uint32_t(tex_list.size());
        if (t->rgba.empty()) {
            const uint64_t id = uint64_t(t->tex_obj) << 32 ^ uint64_t(t->version) << 8 ^ t->tex_type;
            if (auto it = by_identity.find(id); it != by_identity.end() &&
                                               SameIdentity(*tex_list[it->second].tex, *t) &&
                                               tex_list[it->second].tex->width == t->width &&
                                               tex_list[it->second].tex->height == t->height) {
                texs.emplace(t, it->second);
                return;
            }
            texs.emplace(t, index);
            by_identity[id] = index;
            tex_list.push_back({t, most, kNoPixels});
            return;
        }
        const uint64_t hash = PixelHash(*t);
        auto [first, last] = by_pixels.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            Saved& other = tex_list[it->second];
            if (!SamePixels(*other.tex, *t)) continue;
            other.most = std::max(other.most, most);
            if (SameIdentity(*other.tex, *t)) {
                texs.emplace(t, it->second);
            } else {
                texs.emplace(t, index);
                tex_list.push_back({t, most, int32_t(it->second)});
            }
            return;
        }
        texs.emplace(t, index);
        by_pixels.emplace(hash, index);
        tex_list.push_back({t, most, kOwnPixels});
    };
    for (const DrawItem& d : fc.draws) {
        if (geoms.emplace(d.geom.get(), uint32_t(geom_list.size())).second)
            geom_list.push_back(d.geom.get());
        add_tex(d.tex.get(), kMaxSavedTexture);
    }
    for (const ShadeState& s : fc.shades)
        for (const auto& m : s.maps) add_tex(m.get(), kMaxSavedMap);

    Writer w;
    w.Raw(kMagic, sizeof(kMagic));

    size_t sec = w.Begin(kSecFrame, kFrameVersion);
    w.Put<uint64_t>(fc.frame);
    w.Put<uint64_t>(fc.game_frame);
    w.Put<uint32_t>(fc.post_boundary);
    w.Put<uint32_t>(fc.proc_cmds);
    w.Put<uint32_t>(uint32_t(std::size(kFrameCounts)));
    for (const NamedCount& c : kFrameCounts) w.Put<uint32_t>(fc.*c.field);
    w.Put<uint64_t>(fc.world_frame);
    w.End(sec);

    sec = w.Begin(kSecGeometry, kGeometryVersion);
    w.Put<uint32_t>(uint32_t(sizeof(Vertex)));
    w.Put<uint32_t>(uint32_t(geom_list.size()));
    for (const Geometry* g : geom_list) {
        w.Put<uint32_t>(uint32_t(g->verts.size()));
        w.Raw(g->verts.data(), g->verts.size() * sizeof(Vertex));
        w.Put<uint32_t>(uint32_t(g->indices.size()));
        w.Raw(g->indices.data(), g->indices.size() * sizeof(uint16_t));
    }
    w.End(sec);

    sec = w.Begin(kSecTextures, kTexturesVersion);
    w.Put<uint32_t>(uint32_t(tex_list.size()));
    std::vector<Texture> kept(tex_list.size());  // each own-pixel entry as saved
    for (size_t i = 0; i < tex_list.size(); i++) {
        const Saved& t = tex_list[i];
        uint32_t width = t.tex->width, height = t.tex->height, format = t.tex->format;
        if (t.pixels == kOwnPixels) {
            kept[i] = Downsample(*t.tex, t.most);
            width = kept[i].width;
            height = kept[i].height;
        } else if (t.pixels >= 0) {
            width = kept[t.pixels].width;
            height = kept[t.pixels].height;
        }
        w.Put<uint32_t>(width);
        w.Put<uint32_t>(height);
        w.Put<uint32_t>(format);
        w.Put<uint32_t>(t.tex->tex_obj);
        w.Put<uint32_t>(t.tex->tex_type);
        w.Put<uint32_t>(t.tex->version);
        w.Put<int32_t>(t.pixels);
        if (t.pixels == kOwnPixels)
            w.Raw(kept[i].rgba.data(), kept[i].rgba.size() * sizeof(uint32_t));
    }
    w.End(sec);

    sec = w.Begin(kSecShades, kShadesVersion);
    w.Put<uint32_t>(uint32_t(kNumShadeRegs));
    for (uint16_t reg : kShadeRegs) w.Put<uint32_t>(reg);
    w.Put<uint32_t>(uint32_t(kNumShadeMaps));
    w.Put<uint32_t>(uint32_t(fc.shades.size()));
    for (const ShadeState& s : fc.shades) {
        PutShade(w, s);
        for (const auto& m : s.maps) w.Put<int32_t>(m ? int32_t(texs[m.get()]) : -1);
    }
    w.End(sec);

    sec = w.Begin(kSecDraws, kDrawsVersion);
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
        w.Put<int32_t>(d.shade);
        w.Put<uint32_t>(d.target);
        w.Put<int32_t>(d.rect_shader);
        w.Raw(d.rect, sizeof(d.rect));
        w.Put<int32_t>(d.mip_level);
    }
    w.End(sec);

    sec = w.Begin(kSecPasses, kPassesVersion);
    w.Put<uint32_t>(uint32_t(fc.passes.size()));
    for (const Pass& p : fc.passes) {
        for (uint32_t v : {p.tex_obj, p.first_draw, p.draw_count, p.width, p.height, p.tex_type,
                           p.num_mips, p.format, p.clear_flags, p.clear_color})
            w.Put<uint32_t>(v);
        w.Put<float>(p.clear_z);
        w.Raw(p.viewport, sizeof(p.viewport));
        w.Put<uint32_t>(p.cam);
        w.Put<uint32_t>(p.version);
        w.Put<uint64_t>(p.from_frame);
        w.Put<uint32_t>(uint32_t(p.name.size()));
        w.Raw(p.name.data(), p.name.size());
    }
    w.End(sec);

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
    r.end = data.size();
    char magic[8];
    r.Raw(magic, sizeof(magic));
    if (std::memcmp(magic, kMagicV1, sizeof(magic)) == 0) return LoadOld(r, true);
    if (std::memcmp(magic, kMagicV2, sizeof(magic)) == 0) return LoadOld(r, false);
    if (std::memcmp(magic, kMagic, sizeof(magic)) != 0) return nullptr;

    auto fc = std::make_shared<FrameCapture>();
    std::vector<std::shared_ptr<const Geometry>> geoms;
    std::vector<std::shared_ptr<const Texture>> texs;
    bool have_geoms = false, have_texs = false, have_draws = false, shades_skipped = false;
    while (r.ok && r.pos < data.size()) {
        r.end = data.size();
        const uint32_t id = r.Get<uint32_t>();
        const uint32_t version = r.Get<uint32_t>();
        const uint64_t size = r.Get<uint64_t>();
        if (!r.ok || size > data.size() - r.pos) return nullptr;
        const size_t next = r.pos + size_t(size);
        r.end = next;
        const bool core = id == kSecGeometry || id == kSecTextures || id == kSecDraws;
        const uint32_t known = id == kSecFrame      ? kFrameVersion
                               : id == kSecGeometry ? kGeometryVersion
                               : id == kSecTextures ? kTexturesVersion
                               : id == kSecShades   ? kShadesVersion
                               : id == kSecDraws    ? kDrawsVersion
                               : id == kSecPasses   ? kPassesVersion
                                                    : 0;
        if (version > known || version == 0) {
            if (core) return nullptr;
            if (id == kSecShades) shades_skipped = true;
            r.pos = next;
            continue;
        }

        if (id == kSecFrame) {
            fc->frame = r.Get<uint64_t>();
            fc->game_frame = r.Get<uint64_t>();
            fc->post_boundary = r.Get<uint32_t>();
            fc->proc_cmds = r.Get<uint32_t>();
            uint32_t counts;
            if (!r.Count(counts, sizeof(uint32_t))) return nullptr;
            for (uint32_t i = 0; i < counts; i++) {
                const uint32_t v = r.Get<uint32_t>();
                if (i < std::size(kFrameCounts)) fc.get()->*kFrameCounts[i].field = v;
            }
            fc->world_frame = r.end - r.pos >= sizeof(uint64_t) ? r.Get<uint64_t>()
                                                                 : fc->game_frame;
        } else if (id == kSecGeometry) {
            const uint32_t stride = r.Get<uint32_t>();
            uint32_t count;
            if (!stride || !r.Count(count, 8)) return nullptr;
            geoms.resize(count);
            std::vector<uint8_t> vert(stride);
            for (auto& g : geoms) {
                auto geom = std::make_shared<Geometry>();
                uint32_t nv;
                if (!r.Count(nv, stride)) return nullptr;
                geom->verts.resize(nv);
                if (stride == sizeof(Vertex)) {
                    r.Raw(geom->verts.data(), size_t(nv) * sizeof(Vertex));
                } else {
                    // the fields both builds' Vertex have
                    for (Vertex& v : geom->verts) {
                        r.Raw(vert.data(), stride);
                        std::memcpy(&v, vert.data(), std::min<size_t>(stride, sizeof(Vertex)));
                    }
                }
                uint32_t ni;
                if (!r.Count(ni, sizeof(uint16_t))) return nullptr;
                geom->indices.resize(ni);
                r.Raw(geom->indices.data(), size_t(ni) * sizeof(uint16_t));
                g = std::move(geom);
            }
            have_geoms = true;
        } else if (id == kSecTextures) {
            uint32_t count;
            if (!r.Count(count, 28)) return nullptr;
            texs.resize(count);
            for (uint32_t i = 0; i < count; i++) {
                auto tex = std::make_shared<Texture>();
                tex->width = r.Get<uint32_t>();
                tex->height = r.Get<uint32_t>();
                tex->format = r.Get<uint32_t>();
                tex->tex_obj = r.Get<uint32_t>();
                tex->tex_type = r.Get<uint32_t>();
                tex->version = r.Get<uint32_t>();
                const int32_t pixels = r.Get<int32_t>();
                const uint64_t texels = uint64_t(tex->width) * tex->height;
                if (pixels == kOwnPixels) {
                    if (texels * 4 > r.end - r.pos) return nullptr;
                    tex->rgba.resize(size_t(texels));
                    r.Raw(tex->rgba.data(), tex->rgba.size() * sizeof(uint32_t));
                } else if (pixels >= 0) {
                    if (uint32_t(pixels) >= i || texs[pixels]->rgba.size() != texels)
                        return nullptr;
                    tex->rgba = texs[pixels]->rgba;
                } else if (pixels != kNoPixels) {
                    return nullptr;
                }
                texs[i] = std::move(tex);
            }
            have_texs = true;
        } else if (id == kSecShades) {
            uint32_t num_regs;
            if (!r.Count(num_regs, sizeof(uint32_t))) return nullptr;
            std::vector<uint32_t> regs(num_regs);
            for (uint32_t& reg : regs) reg = r.Get<uint32_t>();
            const uint32_t num_maps = r.Get<uint32_t>();
            uint32_t count;
            if (num_maps > 64 || !r.Count(count, 8)) return nullptr;
            fc->shades.resize(count);
            for (ShadeState& s : fc->shades) {
                GetShade(r, s, regs, num_maps);
                for (uint32_t m = 0; m < num_maps; m++) {
                    const int32_t ti = r.Get<int32_t>();
                    if (ti >= int32_t(texs.size())) return nullptr;
                    if (m < uint32_t(kNumShadeMaps) && ti >= 0) s.maps[m] = texs[ti];
                }
                if (!r.ok) return nullptr;
            }
        } else if (id == kSecDraws) {
            if (!have_geoms || !have_texs) return nullptr;
            uint32_t count;
            if (!r.Count(count, 8)) return nullptr;
            fc->draws.resize(count);
            for (DrawItem& d : fc->draws) {
                const uint32_t gi = r.Get<uint32_t>();
                const int32_t ti = r.Get<int32_t>();
                if (gi >= geoms.size() || ti >= int32_t(texs.size())) return nullptr;
                d.geom = geoms[gi];
                if (ti >= 0) d.tex = texs[ti];
                d.world = r.Get<Mat4>();
                d.view_proj = r.Get<Mat4>();
                uint32_t bones;
                if (!r.Count(bones, sizeof(Mat4))) return nullptr;
                d.bones.resize(bones);
                r.Raw(d.bones.data(), d.bones.size() * sizeof(Mat4));
                r.Raw(d.color, sizeof(d.color));
                d.blend = r.Get<int32_t>();
                d.z_mode = r.Get<int32_t>();
                d.prelit = r.Get<uint8_t>() != 0;
                d.alpha_cut = r.Get<uint8_t>() != 0;
                d.alpha_threshold = r.Get<int32_t>();
                d.cam = r.Get<uint32_t>();
                d.mesh = r.Get<uint32_t>();
                d.shade = r.Get<int32_t>();
                d.target = r.Get<uint32_t>();
                d.rect_shader = r.Get<int32_t>();
                r.Raw(d.rect, sizeof(d.rect));
                d.mip_level = r.Get<int32_t>();
                if (!r.ok) return nullptr;
            }
            have_draws = true;
        } else if (id == kSecPasses) {
            uint32_t count;
            if (!r.Count(count, 64)) return nullptr;
            fc->passes.resize(count);
            for (Pass& p : fc->passes) {
                for (uint32_t* v : {&p.tex_obj, &p.first_draw, &p.draw_count, &p.width, &p.height,
                                    &p.tex_type, &p.num_mips, &p.format, &p.clear_flags,
                                    &p.clear_color})
                    *v = r.Get<uint32_t>();
                p.clear_z = r.Get<float>();
                r.Raw(p.viewport, sizeof(p.viewport));
                p.cam = r.Get<uint32_t>();
                p.version = r.Get<uint32_t>();
                p.from_frame = r.Get<uint64_t>();
                uint32_t len;
                if (!r.Count(len, 1)) return nullptr;
                p.name.resize(len);
                r.Raw(p.name.data(), len);
            }
        }
        if (!r.ok) return nullptr;
        r.pos = next;
    }
    if (!r.ok || !have_draws) return nullptr;
    // draws that point past the shades: none if a newer build's shades were
    // skipped, else a broken file
    for (DrawItem& d : fc->draws) {
        if (d.shade < int32_t(fc->shades.size())) continue;
        if (!shades_skipped) return nullptr;
        d.shade = -1;
    }
    for (const Pass& p : fc->passes)
        if (uint64_t(p.first_draw) + p.draw_count > fc->draws.size()) return nullptr;
    return fc;
}

}  // namespace band3::render
