#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "src/Render/gamma_ramp.h"
#include "src/Render/post_params.h"

// Records what RB3 draws each frame, read from guest memory, so the native
// view (native_view.cpp) can draw it without the emulated GPU: the back
// buffer's draws and the texture passes between DxTex::MakeDrawTarget and
// FinishDrawTarget, in draw order.
//
// With native_view_record_targets, texture passes are recorded even while
// capture is off: RB3 makes some once (a band's outfits) and samples them all
// session, so each texture's last pass is kept and carried into captures that
// sample it (Pass::from_frame). Without it, those count as
// FrameCapture::rt_missing. RB3 draws on its splash thread at boot and its
// main thread after, and frees textures on either, so hook state is behind a
// mutex taken past the early-outs.
//
// Offsets are rb3-xenon's (src/system/rndobj, src/system/rnddx9), checked
// against the recompiled DxMesh::DrawShowing, OnSync, SetTransforms,
// DxTex::MakeDrawTarget, FinishDrawTarget, DxCam::Select and DxRnd::DrawRect
// (out/research/m3_render_targets.md, m3_survey.md).

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
    // tangent xyz, handedness w (+-1), for NORMAL_MAP (shade.hlsli's
    // TextureFrame). Last: capture_file.cpp's GEOM needs Vertex to grow only
    // at the end.
    float tan[4];
};

// A mesh's raw guest vertex and index bytes, for decoding later
// (Geometry::deferred, DecodeGeometryBytes). `faces`: the game thread's answer
// to whether it keeps a face, which indices can't tell
struct DeferredGeometry {
    std::once_flag once;
    // set once decoded (deferred_decode.h's GatherPending)
    std::atomic<bool> done{false};
    std::vector<uint8_t> vb, ib;
    uint32_t num_verts = 0, num_indices = 0;
    bool faces = false;
};

struct Geometry {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
    // verts carry the mesh's tangents; false for geometry band3 builds
    // (particles, DrawRect quads) and old captures, whose normal maps the
    // renderers leave out
    bool tangents = false;
    // A vertex buffer's mesh: verts and indices are decoded once from the
    // copied bytes by whoever takes a capture with it first
    // (deferred_decode.h). Null for other geometry and loaded captures.
    std::shared_ptr<DeferredGeometry> deferred;
};

// Corner k (0..3) of a particle's quad, as RB3's particle VS builds it from
// one vertex per particle (2E5F05321D973646, instrs 11-28 and 61-72): (u, v)
// is (0,0) (0,1) (1,1) (1,0), X = (2u-1) size, Y = (2v-1) size, and
//   P' = P + 2w (sin a R + cos a U) + (X cos a - Y sin a) R - (X sin a + Y cos a) U
// with R and U VS c47 and c48 (camera right and up, scaled independently),
// a the angle and w the swing arm. VS c20/c21 transform uv as any texgen.
inline void ParticleCorner(const float p[3], const float right[3], const float up[3], float size,
                           float angle, float swing, int k, float out[3], float uv[2]) {
    const float u = (k == 2 || k == 3) ? 1.0f : 0.0f;
    const float v = (k == 1 || k == 2) ? 1.0f : 0.0f;
    const float x = (2 * u - 1) * size, y = (2 * v - 1) * size;
    const float sn = std::sin(angle), cs = std::cos(angle);
    const float along_r = 2 * swing * sn + x * cs - y * sn;
    const float along_u = 2 * swing * cs - (x * sn + y * cs);
    for (int i = 0; i < 3; i++) out[i] = p[i] + along_r * right[i] + along_u * up[i];
    uv[0] = u;
    uv[1] = v;
}

// A particle's colour as DxParticleSys::DrawParticles packs it
// (band3_recomp.139.cpp): each channel * 255 in single precision, fctidz
// (toward zero; NaN and below -2^63 give 0x8000000000000000, 2^63 and up
// 0x7FFF...), rlwimi keeps the low byte. No clamp: alpha -0.004 past a
// particle's life packs to 0xFF, nearly opaque, as in the game. RGBA8, R low.
inline uint32_t ParticleColor(const float col[4]) {
    uint32_t rgba = 0;
    for (int i = 0; i < 4; i++) {
        const float c = col[i] * 255.0f;
        int64_t v = INT64_MIN;
        if (c >= 9223372036854775808.0f) {
            v = INT64_MAX;
        } else if (c > -9223372036854775808.0f) {  // false for NaN
            v = int64_t(c);
        }
        rgba |= (uint32_t(uint64_t(v)) & 0xffu) << (8 * i);
    }
    return rgba;
}

// RndTex::Type (tex+0x48) values RB3 draws at runtime rather than loads:
// kRendered and its variants (0x22 NoZ, 0x42 shadow map, 0xA2, 0x122),
// back-buffer snapshots (8, 0x18), device textures (0x1000, DxRnd's pre/post)
inline bool IsRenderedType(uint32_t type) {
    return (type & 2) || (type & 8) || type == 0x1000;
}
// of those, the ones texture passes draw into
inline bool IsPassTargetType(uint32_t type) { return (type & 2) != 0; }

// A texture fetch constant's sampling state (xenos.h's xe_gpu_texture_fetch_t
// dwords 0, 3, 4, 5; guest_formats.h's DecodeSampler). The default, used for
// old captures: nearest, wrapping, level 0 only (filtered 0).
struct TexSampler {
    // Xenos ClampMode per axis: 0 repeat, 1 mirrored repeat, 2 clamp to edge,
    // 3 mirror once then edge, 4/5 halfway (drawn as 2/3), 6 border, 7 mirror
    // once then border
    uint8_t clamp_x = 0, clamp_y = 0;
    // 0 point, 1 linear
    uint8_t mag_linear = 0, min_linear = 0;
    // between levels: 0 nearest, 1 linear, 2 mip_min alone
    uint8_t mip = 2;
    // max probes along the footprint's long axis (1 isotropic, 2..16)
    uint8_t aniso = 1;
    uint8_t mip_min = 0, mip_max = 0;
    // 0 transparent black, 1 opaque white
    uint8_t border_white = 0;
    // 1: the game's sampler as above; 0: the old nearest at level 0
    uint8_t filtered = 0;
    // explicit zero padding: a capture saves the struct's bytes
    uint8_t unused[2] = {};
    float lod_bias = 0;  // added to the LOD, in levels
};
static_assert(sizeof(TexSampler) == 16, "TexSampler has no padding");

// A texture's raw guest bytes and fetch constant, for decoding later
// (Texture::deferred): base level (guest_formats.h's BaseLevelBytes) and mip
// chain (MipChainBytes; empty if none or unreadable)
struct DeferredPixels {
    std::once_flag once;
    // set once decoded (deferred_decode.h's GatherPending)
    std::atomic<bool> done{false};
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mips;
    uint32_t fetch[6] = {};
};

// A block-compressed texture's blocks as the GPU takes them (Texture::blocks):
// Xenos DXT1, DXT2_3, DXT4_5 or DXN (`format` 18, 19, 20, 49; BC1/2/3/5),
// untiled and endian-swapped, ceil(w/4) x ceil(h/4) blocks of 8 or 16 bytes
// per level (guest_formats.h's UntileLevelBlocks). rgba_once guards the
// texture's rgba and mips, decoded on demand (deferred_decode.h's EnsureRgba).
struct BlockPixels {
    uint32_t format = 0;
    std::vector<uint8_t> level0;
    std::vector<std::vector<uint8_t>> mips;  // levels 1, 2..., as Texture::mips
    std::once_flag rgba_once;
};

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
    // levels 1, 2... (each half the one before, min 1 texel), from guest
    // memory's mip chain (guest_formats.h's DecodeTextureLevels); empty if
    // none or an old capture
    std::vector<std::vector<uint32_t>> mips;
    uint32_t format = 0;         // Xenos TextureFormat, for the stats
    // A texture RB3 draws at runtime (IsRenderedType): its DxTex, type, and
    // the version a draw sampled (passes resolved since it was made); the pass
    // with the same tex_obj and version made it. rgba is empty unless
    // native_view_rt_fallback is guest (right only with
    // --readback_resolve=full, and not always then). All 0 for a loaded texture.
    uint32_t tex_obj = 0;
    uint32_t tex_type = 0;
    uint32_t version = 0;
    // A CPU-loaded or -written texture (e.g. a movie plane): rgba and mips are
    // decoded once from the copied bytes by whoever takes a capture with it
    // first (deferred_decode.h); width, height and format are set from the
    // start. Null for textures the game thread decodes itself (render target
    // guest pixels, the noise map), undecoded formats and loaded captures.
    std::shared_ptr<DeferredPixels> deferred;
    // A deferred block-compressed texture with native_bc_textures on: the GPU
    // samples the blocks directly, and rgba and mips stay empty until
    // EnsureRgba. Set by DecodeDeferred, so read only after it, as rgba. Null
    // otherwise; shared by capture files' copies.
    std::shared_ptr<BlockPixels> blocks;
};

// The float constants a ShadeState keeps, the same numbers from VS and PS:
// every one RB3's material, environment and particle shaders read (from
// --dump_shaders; set in rb3-xenon rndobj/Mat_NG.cpp, Env_NG.cpp), except the
// world transform (VS c92..c94), which DrawItem::world has and which would
// make every draw's state unique.
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
// Then NgSpotlightDrawer's (out/research/spotlight_survey.md 1, 3), kept only
// for its cones (ShaderType 2) and DrawRect blurs (shader 1), zero elsewhere
// so stale values don't split equal states:
//   cones: c10 eye, c25 apex (w 1/length), c26 axis (w length), c27 eye -
//   apex, c28 (w cos^2 of the half angle), c30 camera forward (w -f.eye),
//   c86..c88 cross-section, c89 depth range, c127 fog (and c90 colour, kept
//   already); blurs: c31..c35 taps' uv offsets, c47..c51 their weights
// (the soft-particle buffer's blurs too, and c89 for its particles, which
// fade by the scene's depth: IsSoftParticle)
inline constexpr uint16_t kShadeRegs[] = {
    0,  1,  2,  5,  7,  13, 14, 15, 16, 17, 18, 19, 20,  21,  22,  23,  24,  40,  41,  42,
    43, 47, 48, 49, 53, 54, 55, 63, 64, 65, 66, 67, 68,  69,  80,  81,  82,  83,  84,  85,
    90, 91, 95, 96, 97, 104, 106, 107, 108, 109, 110, 111, 119, 124, 131, 132, 133, 221, 222, 223,
    10, 25, 26, 27, 28, 30, 86, 87, 88, 89, 127, 31, 32, 33, 34, 35, 50, 51};
inline constexpr int kNumShadeRegs = int(sizeof(kShadeRegs) / sizeof(kShadeRegs[0]));
// kShadeRegs from here on are the spotlight drawer's
inline constexpr int kFirstSpotShadeReg = 60;
static_assert(kShadeRegs[kFirstSpotShadeReg] == 10, "the spotlight's registers follow the 60 others");

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
                     kNormDetail = 24, kBillboard = 25, kFadeOut = 26, kNumProj = 28,
                     kCustomVariation = 30, kColorMod = 32, kRimLight = 37, kEnableAO = 38, kToneMapping = 39,
                     kNumPoint = 40, kEnvironMapFalloff = 43, kProjLightMultiply = 44,
                     kSoftParticles = 45, kRefractWorld = 46, kPointCubeTex = 48,
                     kEnvironMapSpecMask = 49, kIntensify = 53;
}  // namespace shader_opt

// A draw's material-pass shader inputs, read from the D3D device's constant
// shadow after the draw. Plain data, zeroed (padding included) before
// filling, so equal ones can be shared and a .cap can store the bytes.
struct ShadeInputs {
    // RndShader::Cache's last option word and ShaderType (18 standard, 12
    // multimesh, 14 particles; -1 before the first)
    uint64_t options;
    int32_t shader_type;
    uint32_t env;   // RndEnviron::sCurrent
    float eye[3];   // the camera's world position (WorldXfm translation)
    float vs[kNumShadeRegs][4];
    float ps[kNumShadeRegs][4];
    // rb3-xenon rndobj/BaseMaterial.h; kDefaultMaterial for none (NoMaterial)
    uint32_t mat;
    // +0xac: the next pass's material, its own draw (in old captures it was
    // folded into this draw, with these constants)
    uint32_t next_pass;
    uint8_t use_environ;  // +0x99: lit at all
    uint8_t intensify;
    uint8_t per_pixel_lit;
    // +0x9c: writes back-buffer alpha whatever its shader (otherwise only
    // PSEUDO_HDR shaders do); 0 in old captures
    uint8_t alpha_write;
    int32_t shader_variation;  // +0x118: 0 none, 1 skin, 2 hair
    uint32_t mat_maps[kNumShadeMaps];  // its DxTex (RndCubeTex for environ) pointers
    // physical base addresses as fetch constants have them, to check against
    // what the device has bound
    uint32_t mat_map_base[kNumShadeMaps];
    uint32_t mat_diffuse_base;
    // fetch constants for s0 and each map the option word samples (zero for
    // the rest, whose bindings would be stale)
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
    // the maps' mip 0 (2D only); null if unbound, a cube or an undecoded
    // format. s5, s1 or s14 bound to a texture-pass target (the shadow map,
    // NgLight's shadow, a head's normal map) is that texture's identity and
    // version instead (Texture::tex_obj), with guest pixels as a diffuse
    // render target has them.
    std::shared_ptr<const Texture> maps[kNumShadeMaps];
    // from fetch_diffuse and fetch; the default (filtered 0) where unbound and
    // in old captures
    TexSampler diffuse_sampler;
    TexSampler samplers[kNumShadeMaps];
};

// one draw: a material pass of a DxMesh::DrawShowing (one for its material,
// one per RndMat::NextPass), one DxMultiMesh instance in one pass, a particle
// system's quads, or a DxRnd::DrawRect quad. Old captures have one draw per
// mesh, with the first pass's material and the last pass's constants.
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
    // the texture its pass draws into (Pass::tex_obj), 0 for the back buffer
    uint32_t target = 0;
    // A DxRnd::DrawRect quad's ShaderType (6 colour fill, 3 mip downsample,
    // 1 blur, 4 DOF, 11 movie, 16 post copy...), -1 for a mesh draw. Its
    // geometry is the quad in clip space (z 0, w 1, uv 0..1 from the top left
    // through the material's texture transform) with world and view_proj
    // identity; rect is x, y, w, h in the target's pixels.
    int32_t rect_shader = -1;
    float rect[4] = {};
    // a FinishDrawTarget mip downsample: the level it makes (1, 2...) from the
    // one before; it samples nothing else, so a renderer can build the mips
    // itself. 0 otherwise.
    int32_t mip_level = 0;
    // The device's cull mode after the draw: low bits of Xenos'
    // PA_SU_SC_MODE_CNTL (kCullFront, kCullBack, kCullFrontIsCw; D3DCULL_CW is
    // 2, D3DCULL_CCW 6). RndMat's cull flag sets CW; RndShader::CheckForceCull
    // overrides it (CCW for mirrored reflections). Cel-shaded outlines rely on
    // it. 0 for band3-built geometry (particles, DrawRect quads), whose
    // winding isn't the game's, and in old captures.
    uint8_t cull = 0;
    // TheRnd's draw mode (kDrawMode*); 0 in old captures, which kept only
    // colour passes
    uint8_t draw_mode = 0;
};

inline constexpr uint8_t kCullFront = 1, kCullBack = 2, kCullFrontIsCw = 4;
// whether this cull mode drops a triangle with that screen winding (y down)
inline bool Culls(uint8_t cull, bool clockwise) {
    const bool front = clockwise == ((cull & kCullFrontIsCw) != 0);
    return (cull & (front ? kCullFront : kCullBack)) != 0;
}

// DxRnd::FinishPostProcess and CopyPostProcess's DrawRect ShaderType: the
// post-processed picture copied to the screen, which post_model.h redoes
inline constexpr int32_t kRectShaderPostCopy = 16;

// A Bink movie frame: Movie::Impl::Draw (rb3-xenon movie/Movie.cpp) binds the
// 8-bit planes Y as s0, cR as s2, cB as s3 and draws a DrawRect quad with
// ShaderType 11 into a TexMovie's texture (Rnd::DrawPreClear) or the back
// buffer (the intro's MoviePanel). Its shade state keeps the chroma planes as
// kMapSpecular and kMapGlow whatever the option word (0); shade.hlsli's
// MovieRgb converts.
inline constexpr int32_t kMovieShader = 11;
inline bool IsMovie(const DrawItem& d) { return d.rect_shader == kMovieShader; }

// The back buffer's draws the renderers draw, all but the post copy (texture
// passes are drawn separately: soft_raster.h's PlanPasses)
inline bool DrawnToBackBuffer(const DrawItem& d) {
    return d.target == 0 && d.rect_shader != kRectShaderPostCopy;
}

// RndTex::Type of RndShadowMap's 512x512 character self-shadow depth
// (out/research/m3_render_targets.md 1.6): RndShadowMap::PrepShadow's light
// camera clears depth to 1, draws the character in draw mode 1 (SKINNED only,
// D3DCULL_CCW), and the world camera's RndCam::Select resolves the depth into
// it; SHADOW_BUFFER draws then read it as s5 (ShadowMapOf)
inline constexpr uint32_t kTexTypeShadowMap = 0x42;

// TheRnd's draw modes: 0 colour, 1 shadow map depth (RndShadowMap::PrepShadow),
// 3 NgLight's shadow casters (NgLight::RenderShadows, no camera: view_proj is
// VS c4..c7, which it uploads itself; no lights or DIFFUSE_MAP, so white
// silhouettes), 6 soft particles (IsSoftParticle), 7 mirrored reflections.
// rb3-xenon numbers those from 3 on one higher than retail.
inline constexpr uint8_t kDrawModeNormal = 0;
inline constexpr uint8_t kDrawModeShadowDepth = 1;
inline constexpr uint8_t kDrawModeShadowCasters = 3;

// RndTex::Type of NgSpotlightDrawer's targets, drawn after post-processing
// starts: the cones' depth volume (640x360) and the fog density map (320x180)
inline constexpr uint32_t kTexTypeDepthVolume = 0xA2;
inline constexpr uint32_t kTexTypeDensityMap = 0x122;

// A spotlight cone: NgSpotlightDrawer::RenderConeDefs draws the beam's proxy
// mesh (no material) with ShaderType 2 into the depth volume
// (shaders/spot_model.hlsli). Kept as a mesh draw with blend Add, no depth,
// tex the cross-section texture (s11) if sampled, and the cone in the shade
// state's spotlight registers.
inline constexpr int32_t kDepthVolumeShader = 2;
inline bool IsSpotCone(const DrawItem& d, const ShadeInputs* s) {
    return s && s->shader_type == kDepthVolumeShader && d.rect_shader < 0;
}

// A mesh pass without a material (RndShader::SelectConfig(null)), which RB3
// draws with TheRnd's default material (white, prelit, unlit, opaque): kept
// with that material's colour, blend and constants and mat kDefaultMaterial
// (scene_capture.cpp's MeshParts). Old captures kept none.
inline constexpr uint32_t kDefaultMaterial = 0xffffffffu;
inline bool NoMaterial(const DrawItem& d, const ShadeInputs* s) {
    return s && s->mat == kDefaultMaterial && d.rect_shader < 0;
}

// A soft particle: after post-processing starts, RndSoftParticleBuffer::DoPost
// draws the queued RndSoftParticles into its 320x180 surface
// (PostConsts::soft_surface) via DxParticleSys::DrawParticles in draw mode 6,
// where RndShaderParticles::CalcShaderOpts adds option bit 45: alpha fades by
// distance in front of the scene's depth (shade.hlsli's SoftFade, camera range
// in PS c89). The buffer is blurred and composited (post_model.h).
inline constexpr int32_t kParticleShader = 14;
inline bool IsSoftParticle(const DrawItem& d, const ShadeInputs* s) {
    return s && s->shader_type == kParticleShader && s->Option(shader_opt::kSoftParticles) &&
           d.rect_shader < 0;
}

// The shadow map a SHADOW_BUFFER draw reads (s5, kTexTypeShadowMap), or null
// (old captures kept s5 as guest k_24_8, not decoded)
inline const Texture* ShadowMapOf(const ShadeState* s) {
    if (!s || !s->Option(shader_opt::kShadowBuffer)) return nullptr;
    const Texture* t = s->maps[kMapProjected].get();
    return t && t->tex_obj && t->tex_type == kTexTypeShadowMap ? t : nullptr;
}

// The projected light's map (s5 of a per-pixel NUM_PROJ draw) when it's a
// texture RB3 draws, or null: NgLight's shadow, drawn each frame by
// NgLight::RenderShadows into a 256x256 rendered-noz texture from the shadow
// casters (draw mode 3) and blurred twice in place (soft_raster.h's
// ShadowCasterPass, spot::SpotBlur)
inline const Texture* ProjectedTargetOf(const ShadeState* s) {
    if (!s || !s->OptionBits(shader_opt::kNumProj, 2) || !s->Option(shader_opt::kPerPixel))
        return nullptr;
    const Texture* t = s->maps[kMapProjected].get();
    return t && t->tex_obj && IsPassTargetType(t->tex_type) && t->tex_type != kTexTypeShadowMap
               ? t
               : nullptr;
}

// kMapNormal or kMapDetailNormal when it's a texture RB3 draws, or null: a
// head's head_wrinkle_output.tex, composed by a texture pass from its
// expressions (guest memory's copy is garbage without --readback_resolve=full)
inline const Texture* MapTargetOf(const ShadeState* s, int map) {
    if (!s || (map != kMapNormal && map != kMapDetailNormal)) return nullptr;
    const Texture* t = s->maps[map].get();
    return t && t->tex_obj && IsPassTargetType(t->tex_type) && t->tex_type != kTexTypeShadowMap
               ? t
               : nullptr;
}

// A run of FrameCapture::draws to one target: the back buffer, or a texture
// between DxTex::MakeDrawTarget and FinishDrawTarget (which resolves a new
// version). Passes don't nest in RB3.
struct Pass {
    uint32_t tex_obj = 0;  // the DxTex drawn into; 0 the back buffer
    uint32_t first_draw = 0;
    uint32_t draw_count = 0;
    // the texture's size, type, mip count and D3DFORMAT (tex+0x4c, +0x50,
    // +0x48, +0x64, +0x74); 0 for the back buffer
    uint32_t width = 0, height = 0, tex_type = 0, num_mips = 0, format = 0;
    // DxCam::Select's clear as D3DCLEAR bits (0x0f colour, 0x30 depth and
    // stencil); 0 if no camera cleared it
    uint32_t clear_flags = 0;
    uint32_t clear_color = 0;  // D3DCOLOR, ARGB
    float clear_z = 0;
    // x, y, w, h in the target's pixels; w 0 when no camera set one
    float viewport[4] = {};
    uint32_t cam = 0;  // the camera that selected it, 0 none
    uint32_t version = 0;  // the texture's version this pass made
    // the game frame (Present count) it was drawn in; earlier than the
    // capture's for a carried pass
    uint64_t from_frame = 0;
    std::string name;  // the texture's name (Hmx::Object), often empty
};

// A back-buffer camera's viewport as DxCam::SetViewport set it on
// RndCam::Select (rb3-xenon rnddx9/Cam.cpp, Rnd.cpp): screen rect (cam+0x2cc)
// clamped to 0..1 times the back buffer's size, truncated to pixels; z range
// (mZRange, cam+0x2c4) as MinZ/MaxZ. RB3 doesn't clear depth between these
// cameras (only DoPostProcess does, for the overlay) but layers them by z
// range: device depth 1 - (MinZ + z/w (MaxZ - MinZ)), reverse Z (SetViewport
// flips MinZ/MaxZ, test GREATER, clear 0) (soft_raster.h's LayoutBackBuffer).
struct CameraView {
    uint32_t cam = 0;
    float viewport[4] = {};  // x, y, w, h in the back buffer's pixels
    uint32_t target_w = 0, target_h = 0;  // the back buffer's size
    float zrange[2] = {0, 1};
};
static_assert(sizeof(CameraView) == 36, "CameraView has no padding: a capture saves its bytes");

// A mesh in the motion blur velocity buffer's object pass
// (out/research/n5_hub_soft.md 3.1): NgPostProc's mMotionBlurDrawList (the
// characters), drawn in draw mode 5 by RndVelocityBuffer::DrawMesh after the
// camera pass (VS 21A0C657F6C70854 skinned, F922317D5AAC4AE6 not; PS
// 39DE58D45328C089: tools/shaders/research/post/check_velocity.py), blend
// SrcAlpha, depth tested and written. Device constants at the draw: VS c0..c3
// this frame's view-projection, c4..c7 the last's (clip x..w = each row dot
// world position); bone palettes c9.. (this frame) and c129.. (last), 3 rows
// per bone; PS c8 the camera's depth range (c89's). Unskinned: one palette
// entry, the mesh's world.
struct VelocityObject {
    std::shared_ptr<const Geometry> geom;
    uint32_t mesh = 0;
    uint8_t cull = 0;     // as DrawItem::cull
    uint8_t skinned = 0;  // the vertices' bones and weights place them
    uint32_t bones = 0;   // palette entries, 1 unskinned
    float view_proj[8][4] = {};
    float depth_range[4] = {};
    // bones * 3 rows of this frame's palette, then the last frame's
    std::vector<float> rows;  // 4 floats a row
};

struct FrameCapture {
    uint64_t frame = 0;
    uint64_t game_frame = 0;  // Present calls before this frame's
    std::vector<DrawItem> draws;
    std::vector<ShadeState> shades;  // the draws' distinct ones
    // in draw order, carried ones first; none in old captures (all back buffer)
    std::vector<Pass> passes;
    // DxRnd::DoPostProcess: the first draw after post-processing started
    // (kNoPost if it didn't), and TheRnd's ProcCommands then (1 world, 2 post,
    // 7 all; even/odd rendering alternates 1 and 2)
    static constexpr uint32_t kNoPost = ~0u;
    uint32_t post_boundary = kNoPost;
    uint32_t proc_cmds = 0;
    // read at DoPostProcess (valid 0 if it didn't run or an old capture); a
    // composed frame has its post frame's
    PostParams post;
    PostConsts post_consts;
    // The film grain's noise map (PostParams::noise_map) and its sampler: on a
    // post frame sampler 13's (PostConsts::noise_fetch, bound by
    // NgPostProc::CheckNoise); on a world frame the last post frame's. Null if
    // noise is off, the format isn't decoded, or an old capture.
    std::shared_ptr<const Texture> noise_map;
    TexSampler noise_sampler;
    // the motion blur's object pass in draw order, on post frames; none on a
    // world frame and in old captures
    std::vector<VelocityObject> velocity_objects;
    // With even/odd rendering a frame that draws no world (proc_cmds 2)
    // presents the last one that did, so its capture puts that world before
    // its own draws from post_boundary on (frame_compose.h): composed 1,
    // world_frame that frame's game_frame. Otherwise 0, world_frame its own.
    uint32_t composed = 0;
    uint64_t world_frame = 0;
    // a composed frame's draws [0, composed_world_end) are its world frame's
    // (native_world_ahead leaves out those drawn ahead); 0 if not composed
    uint32_t composed_world_end = 0;
    // 0 for the first frame after capture turned on, which has only the
    // draws since; not saved (capture files hold whole frames)
    uint8_t whole = 1;
    // the presenter's display gamma ramp (gamma_ramp.h), read at the frame's
    // end; kNone in old captures and where unreadable
    GammaRamp gamma;
    uint32_t cams = 0;             // camera selects that drew to the back buffer
    uint32_t skipped_target = 0;   // draws for a camera with a target, but no texture pass open
    // the velocity pass, not a draw (its object pass is velocity_objects)
    uint32_t skipped_velocity = 0;
    // draws in shadow draw modes (1, 3) outside their pass: none expected
    uint32_t skipped_shadow = 0;
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
    // texture passes: own, carried from earlier frames, and dropped for
    // keeping no draws (e.g. the velocity buffer's: draw mode 5)
    uint32_t passes_own = 0;
    uint32_t passes_carried = 0;
    uint32_t passes_empty = 0;
    // pass-target textures sampled (diffuse, and s5 as the shadow map or
    // NgLight's), by (texture, version): rt_filtered if made by a pass
    // dropped because all its draws were; rt_missing if made by no pass the
    // capture has (not kept, made before band3 saw it, or drawn by something
    // not recorded). rt_missing 0: nothing missing the capture could have had.
    uint32_t rt_sampled = 0;
    uint32_t rt_missing = 0;
    uint32_t rt_filtered = 0;
    // texture << 32 | version, to recount over a composed frame
    // (frame_compose.h) and for replay's --list
    std::vector<uint64_t> rt_filtered_keys;
    // back-buffer snapshots and device textures sampled (refraction's
    // pre-process buffer), which no pass makes
    uint32_t rt_snapshots = 0;
    // passes that didn't pair up (Make while one was open, Finish of another
    // texture, frame ending inside one): dropped
    uint32_t passes_unbalanced = 0;
    // a mesh's second or later pass (RndMat::NextPass; once per multimesh),
    // and passes with no material other than spotlight cones, kept with the
    // default material (NoMaterial) where they have geometry
    uint32_t later_passes = 0;
    uint32_t skipped_no_mat = 0;
    // DxMesh::DrawFaces outside DrawShowing, not recorded: RndTexBlender's
    // (the velocity buffer's aren't counted)
    uint32_t faces_elsewhere = 0;
    // TheRnd's clear colour (+0x2c, r g b a) that DxRnd::BeginDrawing clears
    // the back buffer to; has_clear_color 0 in old captures (cleared to
    // 0xff202020)
    uint32_t has_clear_color = 0;
    float clear_color[4] = {};
    // the back-buffer cameras selected, each once; none in old captures,
    // which clear depth per camera (RasterOptions::clear_depth_per_camera)
    std::vector<CameraView> cameras;
    // CaptureProfile's counts on the game's thread from the previous frame's
    // end to this one's (so Present's hook time is the previous frame's), in
    // Hook order, and the game's frame time Present to Present. A composed
    // frame has its post frame's. Not saved: 0 in loaded captures.
    struct Cost {
        static constexpr int kHooks = 7;
        uint64_t hook_ns[kHooks] = {};
        uint32_t draws = 0, new_shades = 0, allocs = 0, bones = 0;
        uint64_t geom_copy_bytes = 0, tex_copy_bytes = 0, tex_decode_bytes = 0;
        uint64_t game_ns = 0;
    };
    Cost cost;
};

// the back-buffer camera `cam`'s view in fc.cameras, or null
inline const CameraView* CameraOf(const FrameCapture& fc, uint32_t cam) {
    for (const CameraView& c : fc.cameras)
        if (c.cam == cam) return &c;
    return nullptr;
}

// Texture-pass recording while capture was off (native_view_record_targets,
// `on` now), since startup: passes drawn, those recorded (the rest target
// textures redrawn every frame or two), their draws, and the game thread's
// time recording (skipped passes aren't timed)
struct PassRecordingStats {
    bool on = false;
    uint64_t passes = 0;
    uint64_t passes_recorded = 0;
    uint64_t draws_recorded = 0;
    double ms = 0;
};
PassRecordingStats GetPassRecordingStats();

// Capture's cost to the game's render thread since startup: each hook's time
// past its early-out (excluding the wait for g_state_mutex), counted whenever
// the hooks work; with native_view_capture_profile, the steps inside them,
// each timed from the previous step's end so they sum to the hooks' time
// (rest: the remainder), at a clock read each. Reported by the harness's
// `native_view stats` (`capture`).
struct CaptureProfile {
    enum Hook {
        kHookMesh,       // DxMesh::DrawShowing and DrawFaces
        kHookMultiMesh,  // DxMultiMesh::DrawShowing, its passes at SelectConfig
        kHookParticles,  // DxParticleSys::DrawParticles
        kHookRect,       // DxRnd::DrawRect
        kHookPass,       // texture passes begun, ended, cleared and forgotten
        kHookPresent,    // the frame's end: FinishFrame, publishing it
        kHookOther,      // camera selects and post-processing's reads
        kNumHooks
    };
    static constexpr const char* kHookNames[kNumHooks] = {
        "mesh", "multimesh", "particles", "rect", "pass", "present", "other"};
    enum Step {
        kStepTarget,        // where a draw goes (Target)
        kStepGeomHit,       // a vertex buffer's geometry found in the cache
        kStepGeomMiss,      // not found: its buffers copied to decode later
        kStepGeomMutable,   // a mutable mesh's CPU verts, decoded each draw
        kStepParticleGeom,  // a particle system's quads
        kStepRectGeom,      // a DrawRect's quad
        kStepItem,          // the material's fields, the view-projection
        kStepTexLookup,     // a loaded texture's key hashed and found
        kStepTexDecode,     // its levels copied to decode later, or decoded
                            // here (render target guest pixels, noise map)
        kStepTexRt,         // a render target's identity and version
        kStepShadeRead,     // the shade's constants and fetch constants read
        kStepShadeRtsScan,  // its maps bound to render targets looked for
        kStepShadeIntern,   // hashed and compared with the frame's
        kStepShadeFill,     // a new one's samplers and maps
        kStepShadeStore,    // a new one kept
        kStepBones,         // a skinned draw's bones (count: bones)
        kStepPush,          // the draw added to its frame or pass
        kStepPassAppend,    // a pass's draws and shades into the frame
        kStepCarry,         // the passes a frame samples carried in
        kStepCompose,       // a post frame composed with its world
        kStepGamma,         // the display gamma ramp read
        kStepPublish,       // the frame published (the one before let go)
        kStepReset,         // the next frame started
        kStepRest,          // the hooks' time no step names
        kNumSteps
    };
    static constexpr const char* kStepNames[kNumSteps] = {
        "target",      "geom_hit",    "geom_miss",      "geom_mutable",  "particle_geom",
        "rect_geom",   "item",        "tex_lookup",     "tex_decode",    "tex_rt",
        "shade_read",  "shade_rts_scan", "shade_intern", "shade_fill",   "shade_store",
        "bones",       "push",        "pass_append",    "carry",         "compose",
        "gamma",       "publish",     "reset",          "rest"};
    uint64_t hook_ns[kNumHooks] = {};
    uint64_t hook_calls[kNumHooks] = {};
    uint64_t step_ns[kNumSteps] = {};
    uint64_t step_calls[kNumSteps] = {};
    bool steps_on = false;  // native_view_capture_profile, now
    uint64_t frames = 0;    // the game's frames (Presents)
    uint64_t captured = 0;  // of those, captured
    // draws added (frames and recorded passes), new shade states, shared
    // objects made (frames, geometry, textures, passes)
    uint64_t draws = 0;
    uint64_t new_shades = 0;
    uint64_t allocs = 0;
    // bytes the game's thread copied to decode later (Geometry::deferred,
    // Texture::deferred), and decoded itself (RGBA with levels: render target
    // guest pixels, the noise map)
    uint64_t geom_copy_bytes = 0;
    uint64_t tex_copy_bytes = 0;
    uint64_t tex_decode_bytes = 0;
    uint64_t bones = 0;
    // deferred decodes off the game's thread (deferred_decode.h): count, time
    // and output bytes (RGBA with levels; Vertex and index)
    uint64_t deferred_decodes = 0;
    uint64_t deferred_decode_us = 0;
    uint64_t deferred_decode_bytes = 0;
    // of those, BC textures kept as blocks (native_bc_textures; bytes the
    // blocks'), those decoded to RGBA anyway for a swizzle the GPU can't do,
    // and block ones later asked for RGBA (EnsureRgba)
    uint64_t deferred_bc_blocks = 0;
    uint64_t deferred_bc_swizzled = 0;
    uint64_t deferred_bc_rgba = 0;
    // cache sizes now: render targets, geometry, loaded textures, maps
    uint64_t rts = 0, geoms = 0, texs = 0, map_texs = 0;
};
static_assert(CaptureProfile::kNumHooks == FrameCapture::Cost::kHooks,
              "a frame's cost has every hook's time");
CaptureProfile GetCaptureProfile();
// what `now` counted since `before` (the sizes and steps_on are now's)
CaptureProfile CaptureProfileSince(const CaptureProfile& now, const CaptureProfile& before);

// capture runs only while acquired; each Acquire is matched by a Release
void AcquireCapture();
void ReleaseCapture();

// For a render check: waits for the next captured frame whose world matches
// the game's picture (frame_compose.h's PresentsCapturedWorld; for frames that
// don't say, one drawing about as much as the previous couple), holds the
// game at its end, waits `settle` for the emulated GPU, runs `while_held` (a
// screenshot) and releases the game (held at most three seconds). Null,
// without running while_held, if no frame came in time. If none of the next
// 30 frames qualifies, takes the last and sets `fell_back`.
std::shared_ptr<const FrameCapture> CaptureHeldFrame(
    const std::function<void()>& while_held, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle = std::chrono::milliseconds(150),
    bool* fell_back = nullptr);

// the latest complete frame, composed with the world before it if it drew
// none (frame_compose.h), or null before the first. Its deferred textures
// and geometry are decoded on the caller's thread if no one has yet.
std::shared_ptr<const FrameCapture> LatestCapture();
// also when it was published (end of DxRnd::Present, where the native
// renderer's latency starts) and the ms this call spent decoding
std::shared_ptr<const FrameCapture> LatestCapture(
    std::chrono::steady_clock::time_point& published, double* decode_ms = nullptr);
// the latest FrameCapture::frame, 0 before the first, without decoding
uint64_t LatestCaptureFrame();

// Advances with each capture published and each WakeCaptureWaiters, so the
// native renderer's worker can sleep: WaitForCapture waits up to `timeout`
// for it to pass `epoch` and returns it.
uint64_t CaptureEpoch();
uint64_t WaitForCapture(uint64_t epoch, std::chrono::nanoseconds timeout);
// wakes WaitForCapture as a capture would (the worker has another reason to draw)
void WakeCaptureWaiters();

// End times of the newest few thousand DxRnd::Presents from `since` on,
// oldest first, and the total since startup (present_model.h's PresentTimes)
std::vector<std::chrono::steady_clock::time_point> GamePresentTimes(
    std::chrono::steady_clock::time_point since);
uint64_t GamePresentCount();

// native_view_rt_fallback: whether render targets also carry guest memory's
// pixels ("guest", default) or only identity ("none"). Never while renderer
// is native: the emulated GPU skips the draws that make them (gpu_skip.h).
bool RtFallbackGuest();

}  // namespace band3::render
