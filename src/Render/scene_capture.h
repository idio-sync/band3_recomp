#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Experimental: records what RB3 draws to the back buffer each frame, read
// straight out of guest memory, so the native view (native_view.cpp) can draw
// it without the emulated GPU.
//
// Offsets are rb3-xenon's (src/system/rndobj, src/system/rnddx9), checked
// against the recompiled DxMesh::DrawShowing, DxMesh::OnSync and
// DxMesh::SetTransforms.

namespace band3::render {

// row vectors, v' = v * M, as Milo and D3D9 use them
struct Mat4 {
    float m[4][4];
};

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint32_t color;  // RGBA8, R in the low byte
    uint8_t bone[4];
    float weight[4];
};

struct Geometry {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
};

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
    uint32_t format = 0;         // Xenos TextureFormat, for the stats
};

// The float constant registers a ShadeState keeps, the same numbers from the
// vertex and the pixel shader's: every one RB3's material, environment and
// particle shaders read (their microcode, from a --dump_shaders run; who sets
// them, rb3-xenon rndobj/Mat_NG.cpp and Env_NG.cpp), but the world transform
// (VS c92..c94), which DrawItem::world has, and would make every draw's state
// its own.
//   c0 colour, c1 ambient, c2 specular (rgb, power), c5 (emissive mult,
//   intensify), c7 bloom luminance / VS view-proj, c13 anisotropy,
//   c14 1 - de_normal, c15 half-pixel, c16..c18 inverse view (eye in .w),
//   c19 specular2, c20..c23 texgen, c24 AO strength, c40..c43 shadow matrix,
//   c47..c49 particles, c53..c55 fade, c63 rim, c64/c65 point light pos and
//   1/(falloff-range), c66 projected light direction, c67/c68 point light
//   colour and range scale, c69 projected light colour, c80..c85 box map
//   (+X -X +Y -Y +Z -Z), c90 fog colour, c91 fog, c95..c97 projected light
//   matrix, c104 fade colour, c106 detail normal, c107/c108 shadow colour and
//   direction, c109..c111 colour xfm, c119 refraction, c124 tone mapping,
//   c131..c133 colour mod, c221..c223 point light cube xfm
inline constexpr uint16_t kShadeRegs[] = {
    0,  1,  2,  5,  7,  13, 14, 15, 16, 17, 18, 19, 20,  21,  22,  23,  24,  40,  41,  42,
    43, 47, 48, 49, 53, 54, 55, 63, 64, 65, 66, 67, 68,  69,  80,  81,  82,  83,  84,  85,
    90, 91, 95, 96, 97, 104, 106, 107, 108, 109, 110, 111, 119, 124, 131, 132, 133, 221, 222, 223};
inline constexpr int kNumShadeRegs = int(sizeof(kShadeRegs) / sizeof(kShadeRegs[0]));

// where register `reg` is in kShadeRegs, or -1 if it isn't kept
constexpr int ShadeRegIndex(int reg) {
    for (int i = 0; i < kNumShadeRegs; i++)
        if (kShadeRegs[i] == reg) return i;
    return -1;
}

// the maps RB3's material shaders sample besides the diffuse texture (s0),
// and their samplers
enum ShadeMap {
    kMapNormal,        // s1, normal map (DXN: x, y)
    kMapSpecular,      // s2, rgb colour, a gloss
    kMapGlow,          // s3, emissive
    kMapEnviron,       // s4, a cube: not decoded
    kMapProjected,     // s5, the projected light's mask or the shadow buffer's depth
    kMapGobo,          // s10, the projected light's texture
    kMapDetailNormal,  // s14
    kMapRim,           // s15
    kNumShadeMaps
};
inline constexpr uint32_t kShadeMapSampler[kNumShadeMaps] = {1, 2, 3, 4, 5, 10, 14, 15};

// ShaderOptions bits (rb3-xenon rndobj/ShaderOptions.h, LSB numbering)
namespace shader_opt {
inline constexpr int kPerPixel = 0, kSpecularMap = 1, kSpecular = 2, kEnvironMap = 3,
                     kDiffuseMap = 4, kNormalMap = 5, kGlowMap = 7, kPrelit = 8, kTexGen = 10,
                     kSkinned = 12, kScreenAligned = 13, kRimLightUnder = 14,
                     kRimLightMap = 15, kRealLights = 16, kApproxLights = 17, kFog = 18,
                     kShadowBuffer = 19, kAnisotropic = 20, kColorXfm = 21, kPseudoHdr = 22,
                     kNormDetail = 24, kFadeOut = 26, kNumProj = 28, kCustomVariation = 30,
                     kColorMod = 32, kRimLight = 37, kEnableAO = 38, kToneMapping = 39,
                     kNumPoint = 40, kEnvironMapFalloff = 43, kProjLightMultiply = 44,
                     kPointCubeTex = 48, kEnvironMapSpecMask = 49, kIntensify = 53;
}  // namespace shader_opt

// What a draw's shader was given, read from the D3D device's constant shadow
// after the draw: the constants and option word of its last material pass.
// Plain data, so a frame's draws can share equal ones and a .cap can keep it
// as it is; zeroed before filling, padding included.
struct ShadeInputs {
    // RndShader::Cache's last option word and ShaderType (18 standard, 12
    // multimesh, 14 particles; -1 before the first)
    uint64_t options;
    int32_t shader_type;
    uint32_t env;   // RndEnviron::sCurrent
    float eye[3];   // the camera's world position (WorldXfm translation)
    float vs[kNumShadeRegs][4];
    float ps[kNumShadeRegs][4];
    // the first pass's material (rb3-xenon rndobj/BaseMaterial.h)
    uint32_t mat;
    uint32_t next_pass;  // +0xac: a second pass, whose constants these would be
    uint8_t use_environ;  // +0x99: lit at all
    uint8_t intensify;
    uint8_t per_pixel_lit;
    uint8_t pad0;
    int32_t shader_variation;  // +0x118: 0 none, 1 skin, 2 hair
    uint32_t mat_maps[kNumShadeMaps];  // its DxTex (RndCubeTex for environ) pointers
    // their textures' base addresses, and the diffuse texture's, physical as
    // the fetch constants have them, to check against what the device has bound
    uint32_t mat_map_base[kNumShadeMaps];
    uint32_t mat_diffuse_base;
    // the texture fetch constants bound to s0 and to each map's sampler, for
    // the maps the option word samples (zero for the others, whose binding
    // would be a stale one)
    uint32_t fetch_diffuse[6];
    uint32_t fetch[kNumShadeMaps][6];

    // a register's value, or null if it isn't kept
    const float* Vs(int reg) const {
        const int i = ShadeRegIndex(reg);
        return i < 0 ? nullptr : vs[i];
    }
    const float* Ps(int reg) const {
        const int i = ShadeRegIndex(reg);
        return i < 0 ? nullptr : ps[i];
    }
    bool Option(int bit) const { return (options >> bit) & 1; }
    uint32_t OptionBits(int bit, int count) const {
        return uint32_t(options >> bit) & ((1u << count) - 1);
    }
};

struct ShadeState : ShadeInputs {
    // the maps' mip 0 (2D only), null where none was bound, it's a cube or its
    // format isn't decoded
    std::shared_ptr<const Texture> maps[kNumShadeMaps];
};

// one mesh draw that reached the back buffer (first material pass): a
// DxMesh::DrawShowing, one DxMultiMesh instance, or a particle system's quads
struct DrawItem {
    std::shared_ptr<const Geometry> geom;
    std::shared_ptr<const Texture> tex;
    Mat4 world;
    Mat4 view_proj;
    std::vector<Mat4> bones;  // skinned when not empty; world is unused then
    float color[4];
    int blend;   // RndMat::Blend
    int z_mode;  // RndMat::ZMode
    bool prelit;
    bool alpha_cut;
    int alpha_threshold;
    uint32_t cam;
    uint32_t mesh;
    int32_t shade = -1;  // in FrameCapture::shades; -1 none (a capture from before them)
};

struct FrameCapture {
    uint64_t frame = 0;
    std::vector<DrawItem> draws;
    std::vector<ShadeState> shades;  // the draws' distinct ones
    uint32_t cams = 0;             // camera selects that drew to the back buffer
    uint32_t skipped_target = 0;   // mesh draws into render targets
    uint32_t skipped_velocity = 0; // motion blur velocity pass
    uint32_t skipped_shadow = 0;   // shadow passes (draw modes 1 and 3)
    uint32_t skipped_draw_mode = 0; // other passes that aren't the colour one
    uint32_t skipped_no_geom = 0;  // no material, buffers or faces
    uint32_t mutable_meshes = 0;   // drawn from CPU verts
    uint32_t multimesh_instances = 0;
    uint32_t particles = 0;
    uint32_t textured = 0;
    uint32_t untextured_format = 0; // texture present in a format not decoded
    uint32_t geom_cached = 0;
    uint32_t tex_cached = 0;
    // ShadeState maps the option word samples: decoded, cubes, other formats
    uint32_t maps_decoded = 0;
    uint32_t maps_cube = 0;
    uint32_t maps_other_format = 0;
};

// capture costs a little every frame, so it only runs while something wants
// it: each Acquire is matched by a Release
void AcquireCapture();
void ReleaseCapture();

// For a render check: waits for the next full frame captured (RB3 alternates
// them with overlay-only frames; the request learns the difference over two
// frames), holds the game at the end of it, waits
// `settle` for the emulated GPU to show it, runs `while_held` (a screenshot of
// the same frame) and lets the game go on. The game is never held more than
// three seconds. Null, without running while_held, if no frame came in time.
std::shared_ptr<const FrameCapture> CaptureHeldFrame(
    const std::function<void()>& while_held, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle = std::chrono::milliseconds(150));

// the latest complete frame, or null before the first
std::shared_ptr<const FrameCapture> LatestCapture();

}  // namespace band3::render
