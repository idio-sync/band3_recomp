#include "src/Render/scene_capture.h"

#include <rex/dbg.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "generated/band3_init.h"

// See scene_capture.h.

extern "C" void __imp__RndCam__Select(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxMesh__DrawShowing(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__Present(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxMultiMesh__DrawShowing(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxParticleSys__DrawParticles(PPCContext& ctx, uint8_t* base);

namespace band3::render {
namespace {

// RndMesh and DxMesh (rb3-xenon rndobj/Mesh.h, rnddx9/Mesh.h)
constexpr uint32_t kMesh_WorldXfm = 0x24 + 0x5c;  // RndTransformable at 0x24
constexpr uint32_t kMesh_Verts = 0xd8;             // VertVector {Vert*, n, capacity}
constexpr uint32_t kMesh_Faces = 0xe4;             // std::vector<Face>
constexpr uint32_t kMesh_Mat = 0xf0 + 8;           // ObjPtr<RndMat>, pointer at +8
constexpr uint32_t kMesh_GeomOwner = 0x108 + 8;    // ObjOwnerPtr<RndMesh>
constexpr uint32_t kMesh_BonesBegin = 0x114;
constexpr uint32_t kMesh_BonesEnd = 0x118;
constexpr uint32_t kMesh_Mutable = 0x124;
constexpr uint32_t kDxMesh_NumFaces = 0x160;
constexpr uint32_t kDxMesh_VertexBuffer = 0x164;
constexpr uint32_t kDxMesh_VertexBufferSize = 0x168;
constexpr uint32_t kDxMesh_IndexBuffer = 0x16c;
// RndBone: ObjPtr<RndTransformable>, then Transform mOffset
constexpr uint32_t kBone_Size = 0x4c;
constexpr uint32_t kBone_Trans = 8;
constexpr uint32_t kBone_Offset = 0xc;
constexpr uint32_t kTrans_WorldXfm = 0x5c;
constexpr uint32_t kMaxBones = 256;
// RndMesh::Vert, the CPU copy that mutable meshes keep
constexpr uint32_t kVert_Size = 0x60;
// CompressedVertex_Xbox, what DxMesh::OnSync fills the vertex buffer with
constexpr uint32_t kPackedVert_Size = 36;
// RndMat (rndobj/BaseMaterial.h)
constexpr uint32_t kMat_Blend = 0x28;
constexpr uint32_t kMat_Color = 0x2c;
constexpr uint32_t kMat_ZMode = 0x3c;
constexpr uint32_t kMat_DiffuseTex = 0x8c + 8;
constexpr uint32_t kMat_Prelit = 0x9a;
constexpr uint32_t kMat_AlphaCut = 0x9b;
constexpr uint32_t kMat_AlphaThreshold = 0xa0;
constexpr uint32_t kMat_Fur = 0x104 + 8;
// RndMultiMesh (rndobj/MultiMesh.h)
constexpr uint32_t kMultiMesh_Mesh = 0x24 + 8;    // ObjPtr<RndMesh>
constexpr uint32_t kMultiMesh_Instances = 0x30;  // std::list<Instance>
constexpr uint32_t kMaxInstances = 10000;
// RndParticleSys and RndParticle (rndobj/Part.h), from the full object
constexpr uint32_t kPart_Active = 0x100;
constexpr uint32_t kPart_NumActive = 0x104;
constexpr uint32_t kPart_Mat = 0x1d4 + 8;  // ObjPtr<RndMat>
constexpr uint32_t kParticle_Color = 0x0;
constexpr uint32_t kParticle_Pos = 0x20;
constexpr uint32_t kParticle_Size = 0x48;
constexpr uint32_t kParticle_Next = 0x5c;
constexpr uint32_t kMaxParticles = 16000;  // four verts each, u16 indices
// DxTex (rnddx9/Tex.h)
constexpr uint32_t kDxTex_Texture = 0x78;
// RndCam (rndobj/Cam.h)
constexpr uint32_t kCam_TargetTex = 0x2dc + 8;
constexpr uint32_t kCam_ViewProj = 0x2ec;  // Hmx::Matrix4, written by DxCam::Select
// XDK D3D resources (rb3-xenon xdk/d3d9i/d3d9.h)
constexpr uint32_t kD3DVertexBuffer_Fetch = 0x18;
constexpr uint32_t kD3DIndexBuffer_Address = 0x18;
constexpr uint32_t kD3DIndexBuffer_Size = 0x1c;
constexpr uint32_t kD3DBaseTexture_Fetch = 0x1c;
// DxMesh::DrawShowing draws through RndVelocityBuffer when this is 5
constexpr uint32_t kDrawModeHolder = 0x82C76B68;
constexpr uint32_t kDrawMode = 0xfc;
constexpr uint32_t kDrawModeVelocity = 5;

constexpr uint32_t kMaxBufferBytes = 64u << 20;
constexpr uint32_t kMaxTextureSize = 4096;

uint32_t Be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
uint16_t Be16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
float BeF32(const uint8_t* p) {
    const uint32_t u = Be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// guest memory, through the same macros the recompiled code uses
struct Guest {
    uint8_t* base;
    uint32_t U32(uint32_t a) const { return REX_LOAD_U32(a); }
    uint16_t U16(uint32_t a) const { return REX_LOAD_U16(a); }
    uint8_t U8(uint32_t a) const { return REX_LOAD_U8(a); }
    float F32(uint32_t a) const {
        const uint32_t u = U32(a);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    const uint8_t* Raw(uint32_t a) const { return REX_RAW_ADDR(a); }
};

// Addresses in D3D resources and fetch constants are either physical or, as
// RB3's mostly are, the CPU's 0xA0000000+ view of physical memory. Read the
// latter through the CPU view: the 0xE0000000 range sits 0x1000 further on in
// host memory than TranslatePhysical would put it.
const uint8_t* GpuHost(const Guest& g, uint32_t address) {
    if (address >= 0xA0000000u) return g.Raw(address);
    auto* memory = rex::system::kernel_memory();
    if (!memory) return nullptr;
    return memory->TranslatePhysical(address);
}

Mat4 Identity() {
    Mat4 r{};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// Transform: Matrix3 rows x, y, z then translation v, each a 16-byte Vector3
Mat4 ReadXfm(const Guest& g, uint32_t a) {
    Mat4 r{};
    for (int row = 0; row < 4; row++) {
        for (int c = 0; c < 3; c++) r.m[row][c] = g.F32(a + row * 0x10 + c * 4);
        r.m[row][3] = row == 3 ? 1.0f : 0.0f;
    }
    return r;
}

Mat4 ReadMatrix4(const Guest& g, uint32_t a) {
    Mat4 r{};
    for (int row = 0; row < 4; row++)
        for (int c = 0; c < 4; c++) r.m[row][c] = g.F32(a + (row * 4 + c) * 4);
    return r;
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1;
    const uint32_t exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ff;
    float v;
    if (exp == 0) {
        v = std::ldexp(float(mant), -24);
    } else if (exp == 31) {
        v = mant ? NAN : INFINITY;
    } else {
        v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    }
    return sign ? -v : v;
}

float Dec10(uint32_t bits) {
    int s = int(bits & 0x3ff);
    if (s & 0x200) s -= 0x400;
    return std::max(-1.0f, float(s) / 511.0f);
}

uint32_t Fnv(const uint8_t* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// CompressedVertex_Xbox, as DxMesh's vertex declaration reads it
Vertex DecodePacked(const uint8_t* p) {
    Vertex v{};
    for (int i = 0; i < 3; i++) v.pos[i] = BeF32(p + i * 4);
    const uint32_t argb = Be32(p + 12);
    v.color = ((argb >> 16) & 0xff) | (((argb >> 8) & 0xff) << 8) | ((argb & 0xff) << 16) |
              (argb & 0xff000000u);
    const uint32_t uv = Be32(p + 16);
    v.uv[0] = HalfToFloat(uint16_t(uv >> 16));
    v.uv[1] = HalfToFloat(uint16_t(uv & 0xffff));
    const uint32_t n = Be32(p + 20);
    v.nrm[0] = Dec10(n);
    v.nrm[1] = Dec10(n >> 10);
    v.nrm[2] = Dec10(n >> 20);
    const uint32_t w = Be32(p + 28);
    float sum = 0;
    for (int i = 0; i < 3; i++) {
        v.weight[i] = float((w >> (10 * i)) & 0x3ff) / 1023.0f;
        sum += v.weight[i];
    }
    v.weight[3] = std::max(0.0f, 1.0f - sum);
    const uint32_t bi = Be32(p + 32);
    for (int i = 0; i < 4; i++) v.bone[i] = uint8_t(bi >> (8 * i));
    return v;
}

// RndMesh::Vert in guest memory
Vertex DecodeCpuVert(const Guest& g, uint32_t a) {
    Vertex v{};
    for (int i = 0; i < 3; i++) {
        v.pos[i] = g.F32(a + i * 4);
        v.nrm[i] = g.F32(a + 0x10 + i * 4);
    }
    for (int i = 0; i < 4; i++) {
        v.weight[i] = g.F32(a + 0x20 + i * 4);
        v.bone[i] = uint8_t(g.U16(a + 0x48 + i * 2));
    }
    uint32_t rgba = 0;
    for (int i = 0; i < 4; i++) {
        const float c = std::clamp(g.F32(a + 0x30 + i * 4), 0.0f, 1.0f);
        rgba |= uint32_t(c * 255.0f + 0.5f) << (8 * i);
    }
    v.color = rgba;
    v.uv[0] = g.F32(a + 0x40);
    v.uv[1] = g.F32(a + 0x44);
    return v;
}

// ---------------------------------------------------------------------------
// textures

// Xenos tiled 2D addressing (x, y and pitch in blocks), as Xenia computes it
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
    pitch = (pitch + 31) & ~31u;
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpb_log2 + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

void SwapEndian(uint8_t* p, uint32_t n, uint32_t endian) {
    switch (endian) {
        case 1:  // 8in16
            for (uint32_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]);
            break;
        case 2:  // 8in32
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                std::swap(p[i], p[i + 3]);
                std::swap(p[i + 1], p[i + 2]);
            }
            break;
        case 3:  // 16in32
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                std::swap(p[i], p[i + 2]);
                std::swap(p[i + 1], p[i + 3]);
            }
            break;
        default:
            break;
    }
}

struct Rgba {
    uint8_t c[4];
};

void Rgb565(uint16_t v, Rgba& out) {
    out.c[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
    out.c[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
    out.c[2] = uint8_t((v & 31) * 255 / 31);
    out.c[3] = 255;
}

// the colour half of a DXT block; four_colour forces DXT3/5 behaviour
void DecodeColorBlock(const uint8_t* b, bool four_colour, Rgba out[16]) {
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
    const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
    Rgba pal[4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (four_colour || c0 > c1) {
        for (int i = 0; i < 3; i++) {
            pal[2].c[i] = uint8_t((2 * pal[0].c[i] + pal[1].c[i]) / 3);
            pal[3].c[i] = uint8_t((pal[0].c[i] + 2 * pal[1].c[i]) / 3);
        }
        pal[2].c[3] = pal[3].c[3] = 255;
    } else {
        for (int i = 0; i < 3; i++) pal[2].c[i] = uint8_t((pal[0].c[i] + pal[1].c[i]) / 2);
        pal[2].c[3] = 255;
        pal[3] = Rgba{{0, 0, 0, 0}};
    }
    const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) |
                         (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
}

void DecodeDxt5Alpha(const uint8_t* b, uint8_t out[16]) {
    uint8_t pal[8];
    pal[0] = b[0];
    pal[1] = b[1];
    if (pal[0] > pal[1]) {
        for (int i = 1; i < 7; i++) pal[i + 1] = uint8_t(((7 - i) * pal[0] + i * pal[1]) / 7);
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = uint8_t(((5 - i) * pal[0] + i * pal[1]) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
}

struct FormatInfo {
    uint32_t block;  // block width and height in texels
    uint32_t bpb;    // bytes per block
};

bool GetFormatInfo(uint32_t format, FormatInfo& info) {
    switch (format) {
        case 2: info = {1, 1}; return true;    // k_8
        case 4: info = {1, 2}; return true;    // k_5_6_5
        case 6: info = {1, 4}; return true;    // k_8_8_8_8
        case 10: info = {1, 2}; return true;   // k_8_8
        case 18: info = {4, 8}; return true;   // k_DXT1
        case 19: info = {4, 16}; return true;  // k_DXT2_3
        case 20: info = {4, 16}; return true;  // k_DXT4_5
        default: return false;
    }
}

// texels of one block as the fetch's x, y, z, w components
void DecodeBlock(uint32_t format, const uint8_t* b, Rgba out[16]) {
    switch (format) {
        case 2:
            out[0] = Rgba{{b[0], 0, 0, 255}};
            break;
        case 4: {
            const uint16_t v = uint16_t(b[0] | (b[1] << 8));
            out[0] = Rgba{{uint8_t((v & 31) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63),
                           uint8_t(((v >> 11) & 31) * 255 / 31), 255}};
            break;
        }
        case 6:
            out[0] = Rgba{{b[0], b[1], b[2], b[3]}};
            break;
        case 10:
            out[0] = Rgba{{b[0], b[1], 0, 255}};
            break;
        case 18:
            DecodeColorBlock(b, false, out);
            break;
        case 19:
            DecodeColorBlock(b + 8, true, out);
            for (int i = 0; i < 16; i++) {
                const uint8_t nib = uint8_t((b[i / 2] >> ((i & 1) * 4)) & 0xF);
                out[i].c[3] = uint8_t(nib * 17);
            }
            break;
        case 20: {
            DecodeColorBlock(b + 8, true, out);
            uint8_t a[16];
            DecodeDxt5Alpha(b, a);
            for (int i = 0; i < 16; i++) out[i].c[3] = a[i];
            break;
        }
    }
}

// mip 0 of a 2D texture from its fetch constant, or null for formats not handled
std::shared_ptr<Texture> DecodeTexture(const Guest& g, const uint32_t f[6]) {
    const bool tiled = (f[0] >> 31) & 1;
    const uint32_t pitch_texels = ((f[0] >> 22) & 0x1ff) << 5;
    const uint32_t format = f[1] & 0x3f;
    const uint32_t endian = (f[1] >> 6) & 3;
    const uint32_t base_address = f[1] & 0xfffff000u;
    const uint32_t width = (f[2] & 0x1fff) + 1;
    const uint32_t height = ((f[2] >> 13) & 0x1fff) + 1;
    const uint32_t swizzle = (f[3] >> 1) & 0xfff;
    const uint32_t dimension = (f[5] >> 9) & 3;

    auto tex = std::make_shared<Texture>();
    tex->format = format;
    FormatInfo info;
    if (dimension != 1 || !GetFormatInfo(format, info) || !base_address ||
        width > kMaxTextureSize || height > kMaxTextureSize) {
        return tex;  // empty: format not decoded
    }
    const uint8_t* src = GpuHost(g, base_address);
    if (!src) return tex;

    const uint32_t blocks_x = (width + info.block - 1) / info.block;
    const uint32_t blocks_y = (height + info.block - 1) / info.block;
    const uint32_t pitch_blocks =
        std::max(blocks_x, (std::max(pitch_texels, width) + info.block - 1) / info.block);
    const uint32_t bpb_log2 = info.bpb == 1 ? 0 : info.bpb == 2 ? 1 : info.bpb == 4 ? 2 :
                              info.bpb == 8 ? 3 : 4;
    const uint32_t linear_row = (pitch_blocks * info.bpb + 255) & ~255u;

    tex->width = width;
    tex->height = height;
    tex->rgba.assign(size_t(width) * height, 0);
    uint8_t block[16];
    Rgba texels[16];
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint32_t offset = tiled ? uint32_t(TiledOffset2D(int32_t(bx), int32_t(by),
                                                                   pitch_blocks, bpb_log2))
                                          : by * linear_row + bx * info.bpb;
            std::memcpy(block, src + offset, info.bpb);
            SwapEndian(block, info.bpb, endian);
            DecodeBlock(format, block, texels);
            const uint32_t n = info.block;
            for (uint32_t ty = 0; ty < n; ty++) {
                for (uint32_t tx = 0; tx < n; tx++) {
                    const uint32_t x = bx * n + tx, y = by * n + ty;
                    if (x >= width || y >= height) continue;
                    const Rgba& s = texels[ty * n + tx];
                    uint32_t rgba = 0;
                    for (int c = 0; c < 4; c++) {
                        const uint32_t sel = (swizzle >> (3 * c)) & 7;
                        const uint8_t v = sel < 4 ? s.c[sel] : sel == 4 ? 0 : 255;
                        rgba |= uint32_t(v) << (8 * c);
                    }
                    tex->rgba[size_t(y) * width + x] = rgba;
                }
            }
        }
    }
    return tex;
}

// ---------------------------------------------------------------------------
// capture state, touched only from the game's render thread

struct GeomEntry {
    uint64_t key;
    std::shared_ptr<const Geometry> geom;
};

struct TexEntry {
    uint64_t key;
    std::shared_ptr<const Texture> tex;  // empty rgba: not decoded
};

struct State {
    std::shared_ptr<FrameCapture> building = std::make_shared<FrameCapture>();
    uint32_t cam = 0;
    bool cam_backbuffer = false;
    bool cam_counted = false;
    bool vp_valid = false;
    Mat4 vp{};
    uint64_t frame = 0;
    std::unordered_map<uint32_t, GeomEntry> geoms;
    std::unordered_map<uint32_t, TexEntry> texs;
};

State& S() {
    static State state;
    return state;
}

std::atomic<bool> g_enabled{false};
// what has capture on: the native view, the dump, a held-frame request
std::mutex g_users_mutex;
int g_users = 0;

// a CaptureHeldFrame request, answered on the game's render thread
struct HeldRequest {
    std::mutex mutex;
    std::condition_variable cv;
    bool armed = false;
    int skip = 0;     // frames begun before capture was on are partial
    int waited = 0;   // frames looked at so far
    size_t most = 0;  // the most draws a frame had so far
    std::shared_ptr<const FrameCapture> frame;
    bool released = true;
};
HeldRequest g_held;
// frames looked at before choosing, to learn how many draws a full one has
constexpr int kFramesToLearn = 2;
// after this many, any frame will do
constexpr int kMaxFramesToWait = 30;
// the game is never held longer than this, even if the requester goes away
constexpr std::chrono::seconds kMaxHold{3};
std::mutex g_latest_mutex;
std::shared_ptr<const FrameCapture> g_latest;

uint64_t Key(std::initializer_list<uint32_t> parts) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t p : parts) h = (h ^ p) * 1099511628211ull;
    return h;
}

std::shared_ptr<const Geometry> CaptureGeometry(const Guest& g, uint32_t geom,
                                                FrameCapture& fc) {
    const uint32_t vb = g.U32(geom + kDxMesh_VertexBuffer);
    const uint32_t ib = g.U32(geom + kDxMesh_IndexBuffer);
    const uint32_t num_faces = g.U32(geom + kDxMesh_NumFaces);

    if (vb && ib && num_faces) {
        const uint32_t fetch0 = g.U32(vb + kD3DVertexBuffer_Fetch);
        const uint32_t fetch1 = g.U32(vb + kD3DVertexBuffer_Fetch + 4);
        const uint32_t vb_phys = fetch0 & ~3u;
        uint32_t vb_bytes = ((fetch1 >> 2) & 0xffffff) * 4;
        const uint32_t vb_size = g.U32(geom + kDxMesh_VertexBufferSize);
        if (vb_size) vb_bytes = std::min(vb_bytes, vb_size);
        const uint32_t ib_addr = g.U32(ib + kD3DIndexBuffer_Address);
        const uint32_t ib_bytes = g.U32(ib + kD3DIndexBuffer_Size);
        if (!vb_phys || !ib_addr || vb_bytes < kPackedVert_Size || vb_bytes > kMaxBufferBytes ||
            ib_bytes > kMaxBufferBytes) {
            return nullptr;
        }
        const uint8_t* vsrc = GpuHost(g, vb_phys);
        const uint8_t* isrc = GpuHost(g, ib_addr);
        if (!vsrc || !isrc) return nullptr;
        const uint32_t num_verts = std::min<uint32_t>(vb_bytes / kPackedVert_Size, 65536);
        const uint32_t num_indices = std::min(num_faces * 3, ib_bytes / 2);

        const uint64_t key = Key({vb_phys, vb_bytes, ib_addr, ib_bytes, num_faces,
                                  Fnv(vsrc, std::min<uint32_t>(vb_bytes, 256)),
                                  Fnv(isrc, std::min<uint32_t>(num_indices * 2, 64))});
        auto it = S().geoms.find(geom);
        if (it != S().geoms.end() && it->second.key == key) {
            fc.geom_cached++;
            return it->second.geom;
        }
        auto out = std::make_shared<Geometry>();
        out->verts.resize(num_verts);
        for (uint32_t i = 0; i < num_verts; i++)
            out->verts[i] = DecodePacked(vsrc + i * kPackedVert_Size);
        out->indices.reserve(num_indices);
        for (uint32_t i = 0; i + 2 < num_indices; i += 3) {
            const uint16_t a = Be16(isrc + i * 2), b = Be16(isrc + i * 2 + 2),
                           c = Be16(isrc + i * 2 + 4);
            if (a >= num_verts || b >= num_verts || c >= num_verts) continue;
            out->indices.insert(out->indices.end(), {a, b, c});
        }
        S().geoms[geom] = GeomEntry{key, out};
        return out;
    }

    // mutable meshes keep their CPU verts and faces instead of a vertex buffer
    if (g.U32(geom + kMesh_Mutable) == 0) return nullptr;
    const uint32_t verts = g.U32(geom + kMesh_Verts);
    const uint32_t num_verts = std::min<uint32_t>(g.U32(geom + kMesh_Verts + 4), 65536);
    const uint32_t faces = g.U32(geom + kMesh_Faces);
    const uint32_t faces_end = g.U32(geom + kMesh_Faces + 4);
    if (!verts || !num_verts || !faces || faces_end <= faces) return nullptr;
    const uint32_t num_faces_cpu = std::min<uint32_t>((faces_end - faces) / 6, 1u << 20);
    auto out = std::make_shared<Geometry>();
    out->verts.resize(num_verts);
    for (uint32_t i = 0; i < num_verts; i++)
        out->verts[i] = DecodeCpuVert(g, verts + i * kVert_Size);
    out->indices.reserve(num_faces_cpu * 3);
    for (uint32_t i = 0; i < num_faces_cpu; i++) {
        const uint16_t a = g.U16(faces + i * 6), b = g.U16(faces + i * 6 + 2),
                       c = g.U16(faces + i * 6 + 4);
        if (a >= num_verts || b >= num_verts || c >= num_verts) continue;
        out->indices.insert(out->indices.end(), {a, b, c});
    }
    fc.mutable_meshes++;
    return out;
}

std::shared_ptr<const Texture> CaptureTexture(const Guest& g, uint32_t tex_obj,
                                              FrameCapture& fc) {
    const uint32_t d3d = g.U32(tex_obj + kDxTex_Texture);
    if (!d3d) return nullptr;
    uint32_t f[6];
    for (int i = 0; i < 6; i++) f[i] = g.U32(d3d + kD3DBaseTexture_Fetch + i * 4);
    const uint32_t base_address = f[1] & 0xfffff000u;
    const uint8_t* src = base_address ? GpuHost(g, base_address) : nullptr;
    const uint64_t key = Key({f[0], f[1], f[2], f[3], f[5], src ? Fnv(src, 64) : 0});
    auto it = S().texs.find(d3d);
    std::shared_ptr<const Texture> tex;
    if (it != S().texs.end() && it->second.key == key) {
        fc.tex_cached++;
        tex = it->second.tex;
    } else {
        tex = DecodeTexture(g, f);
        S().texs[d3d] = TexEntry{key, tex};
    }
    if (tex->rgba.empty()) {
        fc.untextured_format++;
        return nullptr;
    }
    fc.textured++;
    return tex;
}

// false (and counted) unless the current camera draws to the back buffer
// outside the velocity pass
bool ToBackBuffer(const Guest& g, State& s, FrameCapture& fc) {
    if (!s.cam || !s.cam_backbuffer) {
        fc.skipped_target++;
        return false;
    }
    const uint32_t holder = g.U32(kDrawModeHolder);
    if (holder && g.U32(holder + kDrawMode) == kDrawModeVelocity) {
        fc.skipped_velocity++;
        return false;
    }
    if (!s.vp_valid) {
        s.vp = ReadMatrix4(g, s.cam + kCam_ViewProj);
        s.vp_valid = true;
    }
    if (!s.cam_counted) {
        fc.cams++;
        s.cam_counted = true;
    }
    return true;
}

// a draw of `geometry` with material `mat`, for the current camera
DrawItem MakeItem(const Guest& g, State& s, FrameCapture& fc, uint32_t mat, uint32_t owner,
                  std::shared_ptr<const Geometry> geometry) {
    DrawItem item;
    item.geom = std::move(geometry);
    item.world = Identity();
    item.view_proj = s.vp;
    for (int i = 0; i < 4; i++) item.color[i] = g.F32(mat + kMat_Color + i * 4);
    item.blend = int(g.U32(mat + kMat_Blend));
    item.z_mode = int(g.U32(mat + kMat_ZMode));
    item.prelit = g.U8(mat + kMat_Prelit) != 0;
    item.alpha_cut = g.U8(mat + kMat_AlphaCut) != 0;
    item.alpha_threshold = int(g.U32(mat + kMat_AlphaThreshold));
    item.cam = s.cam;
    item.mesh = owner;
    const uint32_t tex = g.U32(mat + kMat_DiffuseTex);
    if (tex) item.tex = CaptureTexture(g, tex, fc);
    return item;
}

// the mesh's material and geometry, or false (and counted) if it draws nothing
bool MeshParts(const Guest& g, FrameCapture& fc, uint32_t mesh, uint32_t& mat,
               std::shared_ptr<const Geometry>& geometry) {
    mat = g.U32(mesh + kMesh_Mat);
    uint32_t geom = g.U32(mesh + kMesh_GeomOwner);
    if (!geom) geom = mesh;
    if (!mat || g.U32(mat + kMat_Fur)) {
        fc.skipped_no_geom++;
        return false;
    }
    geometry = CaptureGeometry(g, geom, fc);
    if (!geometry || geometry->indices.empty()) {
        fc.skipped_no_geom++;
        return false;
    }
    return true;
}

void CaptureMesh(uint8_t* base, uint32_t mesh) {
    State& s = S();
    FrameCapture& fc = *s.building;
    const Guest g{base};
    if (!ToBackBuffer(g, s, fc)) return;
    uint32_t mat;
    std::shared_ptr<const Geometry> geometry;
    if (!MeshParts(g, fc, mesh, mat, geometry)) return;

    DrawItem item = MakeItem(g, s, fc, mat, mesh, std::move(geometry));
    item.world = ReadXfm(g, mesh + kMesh_WorldXfm);
    const uint32_t bones = g.U32(mesh + kMesh_BonesBegin);
    const uint32_t bones_end = g.U32(mesh + kMesh_BonesEnd);
    if (bones && bones_end > bones) {
        const uint32_t n = std::min((bones_end - bones) / kBone_Size, kMaxBones);
        item.bones.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t bone = bones + i * kBone_Size;
            const uint32_t trans = g.U32(bone + kBone_Trans);
            item.bones[i] = trans ? Mul(ReadXfm(g, bone + kBone_Offset),
                                        ReadXfm(g, trans + kTrans_WorldXfm))
                                  : Identity();
        }
    }
    fc.draws.push_back(std::move(item));
}

// DxMultiMesh draws its mesh once per instance, instanced, without going
// through the mesh's DrawShowing; one draw per instance here
void CaptureMultiMesh(uint8_t* base, uint32_t multimesh) {
    State& s = S();
    FrameCapture& fc = *s.building;
    const Guest g{base};
    const uint32_t mesh = g.U32(multimesh + kMultiMesh_Mesh);
    if (!mesh || !ToBackBuffer(g, s, fc)) return;
    uint32_t mat;
    std::shared_ptr<const Geometry> geometry;
    if (!MeshParts(g, fc, mesh, mat, geometry)) return;

    const DrawItem proto = MakeItem(g, s, fc, mat, mesh, std::move(geometry));
    // std::list with its sentinel node inline: next at +0, the Instance at +8
    const uint32_t head = multimesh + kMultiMesh_Instances;
    uint32_t n = 0;
    for (uint32_t node = g.U32(head); node && node != head && n < kMaxInstances;
         node = g.U32(node), n++) {
        DrawItem item = proto;
        item.world = ReadXfm(g, node + 8);
        fc.draws.push_back(std::move(item));
    }
    fc.multimesh_instances += n;
}

// DxParticleSys's vertex fill: one camera-facing quad per active particle
void CaptureParticles(uint8_t* base, uint32_t sys) {
    State& s = S();
    FrameCapture& fc = *s.building;
    const Guest g{base};
    const uint32_t mat = g.U32(sys + kPart_Mat);
    if (!mat || !g.U32(sys + kPart_NumActive) || !ToBackBuffer(g, s, fc)) return;

    // the camera's right (x) and up (z) axes; Milo cameras look down +y
    const Mat4 cam = ReadXfm(g, s.cam + kTrans_WorldXfm);
    auto geom = std::make_shared<Geometry>();
    uint32_t n = 0;
    for (uint32_t p = g.U32(sys + kPart_Active); p && n < kMaxParticles;
         p = g.U32(p + kParticle_Next), n++) {
        float pos[3], col[4];
        for (int i = 0; i < 3; i++) pos[i] = g.F32(p + kParticle_Pos + i * 4);
        for (int i = 0; i < 4; i++)
            col[i] = std::clamp(g.F32(p + kParticle_Color + i * 4), 0.0f, 1.0f);
        const float half = g.F32(p + kParticle_Size) * 0.5f;
        uint32_t rgba = 0;
        for (int i = 0; i < 4; i++) rgba |= uint32_t(col[i] * 255.0f + 0.5f) << (8 * i);
        const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const uint16_t first = uint16_t(geom->verts.size());
        for (int k = 0; k < 4; k++) {
            Vertex v{};
            for (int i = 0; i < 3; i++) {
                v.pos[i] = pos[i] + (cam.m[0][i] * corner[k][0] + cam.m[2][i] * corner[k][1]) * half;
                v.nrm[i] = -cam.m[1][i];
            }
            v.uv[0] = uv[k][0];
            v.uv[1] = uv[k][1];
            v.color = rgba;
            geom->verts.push_back(v);
        }
        geom->indices.insert(geom->indices.end(),
                             {first, uint16_t(first + 1), uint16_t(first + 2), first,
                              uint16_t(first + 2), uint16_t(first + 3)});
    }
    if (geom->indices.empty()) return;
    fc.particles += n;
    DrawItem item = MakeItem(g, s, fc, mat, sys, std::move(geom));
    item.prelit = true;  // the particle colour is the vertex colour
    fc.draws.push_back(std::move(item));
}

// RB3 alternates frames that draw the scene with ones that only redraw the
// overlay (a handful of draws, sometimes from two cameras); a request learns
// what a full frame draws over a couple of frames, then takes the next one
// that draws about as much
void HoldIfRequested(const std::shared_ptr<const FrameCapture>& frame) {
    std::unique_lock lock(g_held.mutex);
    if (!g_held.armed) return;
    if (g_held.skip > 0) {
        g_held.skip--;
        return;
    }
    const size_t draws = frame->draws.size();
    const bool learning = g_held.waited < kFramesToLearn;
    const bool full = draws > 0 && draws * 10 >= g_held.most * 9;
    g_held.most = std::max(g_held.most, draws);
    if ((learning || !full) && ++g_held.waited <= kMaxFramesToWait) return;
    g_held.armed = false;
    g_held.frame = frame;
    g_held.released = false;
    g_held.cv.notify_all();
    g_held.cv.wait_for(lock, kMaxHold, [] { return g_held.released; });
    g_held.released = true;
}

void FinishFrame() {
    State& s = S();
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (!s.building->draws.empty()) s.building = std::make_shared<FrameCapture>();
        return;
    }
    s.building->frame = ++s.frame;
    std::shared_ptr<const FrameCapture> done = s.building;
    {
        std::lock_guard lock(g_latest_mutex);
        g_latest = done;
    }
    s.building = std::make_shared<FrameCapture>();
    HoldIfRequested(done);
    s.cam_counted = false;
    // a venue change leaves stale entries behind; start over now and then
    if (s.geoms.size() > 50000) s.geoms.clear();
    if (s.texs.size() > 20000) s.texs.clear();
}

}  // namespace

void AcquireCapture() {
    std::lock_guard lock(g_users_mutex);
    if (g_users++ == 0) g_enabled.store(true);
}

void ReleaseCapture() {
    std::lock_guard lock(g_users_mutex);
    if (g_users > 0 && --g_users == 0) g_enabled.store(false);
}

std::shared_ptr<const FrameCapture> CaptureHeldFrame(const std::function<void()>& while_held,
                                                     std::chrono::milliseconds timeout,
                                                     std::chrono::milliseconds settle) {
    const bool was_on = g_enabled.load();
    AcquireCapture();
    std::unique_lock lock(g_held.mutex);
    g_held.armed = true;
    g_held.skip = was_on ? 0 : 1;
    g_held.waited = 0;
    g_held.most = 0;
    g_held.frame.reset();
    const bool got = g_held.cv.wait_for(lock, timeout, [] { return g_held.frame != nullptr; });
    std::shared_ptr<const FrameCapture> frame = g_held.frame;
    g_held.armed = false;
    g_held.frame.reset();
    lock.unlock();
    if (got) {
        // the game is held at the end of the frame; give the GPU and presenter
        // time to show it
        std::this_thread::sleep_for(settle);
        while_held();
        lock.lock();
        g_held.released = true;
        g_held.cv.notify_all();
        lock.unlock();
    }
    ReleaseCapture();
    return got ? frame : nullptr;
}

std::shared_ptr<const FrameCapture> LatestCapture() {
    std::lock_guard lock(g_latest_mutex);
    return g_latest;
}

}  // namespace band3::render

using namespace band3::render;

extern "C" REX_FUNC(RndCam__Select) {
    const uint32_t cam = ctx.r3.u32;
    __imp__RndCam__Select(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    State& s = S();
    if (cam != s.cam) s.cam_counted = false;
    s.cam = cam;
    s.vp_valid = false;
    s.cam_backbuffer = REX_LOAD_U32(cam + kCam_TargetTex) == 0;
}

extern "C" REX_FUNC(DxMesh__DrawShowing) {
    const uint32_t mesh = ctx.r3.u32;
    __imp__DxMesh__DrawShowing(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    CaptureMesh(base, mesh);
}

extern "C" REX_FUNC(DxMultiMesh__DrawShowing) {
    const uint32_t multimesh = ctx.r3.u32;
    __imp__DxMultiMesh__DrawShowing(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    CaptureMultiMesh(base, multimesh);
}

extern "C" REX_FUNC(DxParticleSys__DrawParticles) {
    const uint32_t sys = ctx.r3.u32;
    __imp__DxParticleSys__DrawParticles(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    CaptureParticles(base, sys);
}

extern "C" REX_FUNC(DxRnd__Present) {
    SCOPE_profile_cpu_f("RB3 DxRnd::Present");
    __imp__DxRnd__Present(ctx, base);
    FinishFrame();
}
