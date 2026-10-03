#pragma once

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

// Experimental: records what RB3 draws each frame, read straight out of guest
// memory, so the native view (native_view.cpp) can draw it without the
// emulated GPU: the back buffer's draws, and the passes RB3 renders into
// textures (outfit composites, crowd impostors, blurs and the rest) between
// DxTex::MakeDrawTarget and FinishDrawTarget, in the order it drew them.
//
// With native_view_record_targets (off by default), texture passes are
// recorded even while capture is off, cheaply: RB3 makes some once (a band's
// outfits, in the main menu) and samples them for the rest of the session, so
// each texture's last pass is kept and a capture that samples it without
// drawing it again carries a copy (Pass::from_frame). Without it, a capture
// has only the passes drawn while capture is on, and counts the rest it
// samples as missing (FrameCapture::rt_missing); the hooks then cost an
// early-out each. RB3 draws on its splash thread at boot and on its main
// thread after, and frees textures on either, so what the hooks keep is
// behind a mutex they take past their early-outs.
//
// Offsets are rb3-xenon's (src/system/rndobj, src/system/rnddx9), checked
// against the recompiled DxMesh::DrawShowing, DxMesh::OnSync and
// DxMesh::SetTransforms; the texture passes' against DxTex::MakeDrawTarget,
// FinishDrawTarget, DxCam::Select and DxRnd::DrawRect
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
    // the tangent, xyz, and its handedness, w (+-1): what a NORMAL_MAP
    // material's vertex shader builds the normal map's frame from
    // (shaders/shade.hlsli's TextureFrame). Last, as capture_file.cpp's
    // GEOM needs: Vertex only grows at the end.
    float tan[4];
};

struct Geometry {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
    // the verts' tangents are the mesh's (a vertex buffer's or a mutable
    // mesh's): false for geometry band3 builds (particles, DrawRect quads)
    // and in captures from before they were kept, whose normal maps the
    // renderers leave out
    bool tangents = false;
};

// Corner k (0..3) of a particle's quad, as RB3's particle VS builds it from
// one vertex per particle (2E5F05321D973646, instrs 11-28 and 61-72): (u, v)
// is (0,0) (0,1) (1,1) (1,0), X = (2u-1) size, Y = (2v-1) size, and
//   P' = P + 2w (sin a R + cos a U) + (X cos a - Y sin a) R - (X sin a + Y cos a) U
// with R and U VS c47 and c48 (the camera's right and up, each scaled on
// its own: a quad can be narrower than it's wide), a the particle's angle
// and w its swing arm. uv is (u, v), which VS c20/c21 transform as any
// texgen does.
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

// A particle's colour (r, g, b, a floats) as DxParticleSys::DrawParticles
// packs it into its vertex (band3_recomp.139.cpp, both paths): each channel
// times DrawShowing's colour, (1,1,1,1), then 255, single precision; fctidz
// (toward zero, NaN and below -2^63 to 0x8000000000000000, 2^63 and up to
// 0x7FFF...); rlwimi keeps each one's low byte. No clamp: a particle a
// little past the end of its life, alpha -0.004, is -1, 0xFF, drawn nearly
// opaque, as the game draws it. RGBA8 here, R in the low byte.
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

// RndTex::Type (tex+0x48) values that make a texture's pixels something RB3
// draws at runtime rather than loads: kRendered and what's built on it
// (0x22 NoZ, 0x42 shadow map, 0xA2, 0x122), back-buffer snapshots (8, 0x18)
// and device textures (0x1000, DxRnd's pre and post buffers)
inline bool IsRenderedType(uint32_t type) {
    return (type & 2) || (type & 8) || type == 0x1000;
}
// of those, the ones texture passes draw into
inline bool IsPassTargetType(uint32_t type) { return (type & 2) != 0; }

// What a texture fetch constant tells the texture unit to do with a sample
// (xenos.h's xe_gpu_texture_fetch_t: clamp_x/y in dword 0, the filters and
// anisotropy in dword 3, the mip range and LOD bias in dword 4, the border
// colour in dword 5; guest_formats.h's DecodeSampler). The default is the
// one the renderers drew every texture with before captures kept them, and
// draw old captures with: nearest, wrapping, level 0 alone (filtered 0).
struct TexSampler {
    // Xenos ClampMode per axis: 0 repeat, 1 mirrored repeat, 2 clamp to the
    // edge, 3 mirror once then the edge, 4/5 halfway (drawn as 2/3), 6 the
    // border, 7 mirror once then the border
    uint8_t clamp_x = 0, clamp_y = 0;
    // magnification and minification: 0 point, 1 linear
    uint8_t mag_linear = 0, min_linear = 0;
    // between levels: 0 the nearest, 1 linear, 2 the base level (mip_min) alone
    uint8_t mip = 2;
    // the most probes along a footprint's long axis (1 isotropic, 2..16)
    uint8_t aniso = 1;
    // the levels it may read, LOD clamped to them
    uint8_t mip_min = 0, mip_max = 0;
    // the border's colour: 0 transparent black, 1 opaque white
    uint8_t border_white = 0;
    // 1: the game's sampler, sampled as above; 0: the old nearest at level 0
    uint8_t filtered = 0;
    // what would be padding, zero: a capture saves the struct's bytes
    uint8_t unused[2] = {};
    float lod_bias = 0;  // added to the LOD, in levels
};
static_assert(sizeof(TexSampler) == 16, "TexSampler has no padding");

// A movie's plane as capture took it, for decoding later (Texture::deferred):
// its base level's bytes as guest memory held them, and its fetch constant
struct DeferredPixels {
    std::once_flag once;
    std::vector<uint8_t> bytes;
    uint32_t fetch[6] = {};
};

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
    // levels 1, 2... (each half the one before, at least 1 texel), as RB3's
    // mip chain in guest memory has them (guest_formats.h's
    // DecodeTextureLevels), for the samplers that read them; empty where the
    // texture has none or the capture is from before they were kept
    std::vector<std::vector<uint32_t>> mips;
    uint32_t format = 0;         // Xenos TextureFormat, for the stats
    // A texture RB3 draws at runtime (IsRenderedType): its DxTex, its type and
    // the version a draw sampled, how many of its passes had resolved by then
    // (0 none since it was made); the pass that made it is the one with the
    // same tex_obj and version. Its rgba is empty unless native_view_rt_fallback
    // is guest, which decodes what guest memory holds (right only with
    // --readback_resolve=full, and not always then). All 0 for a loaded texture.
    uint32_t tex_obj = 0;
    uint32_t tex_type = 0;
    uint32_t version = 0;
    // A movie's plane (scene_capture.cpp's DecodeCached): the CPU writes it
    // anew for each movie frame, so the game's thread only copies its bytes,
    // and rgba is decoded from them once, by whoever takes the capture first
    // (LatestCapture, CaptureHeldFrame), before they hand it on; width,
    // height and format are set from the start. Null for every other
    // texture, and in captures loaded from a file.
    std::shared_ptr<DeferredPixels> deferred;
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
// Then NgSpotlightDrawer's (out/research/spotlight_survey.md 1 and 3), kept
// for its cones (ShaderType 2) and DrawRect blurs (shader 1) only and zero in
// every other draw's state, where they'd be stale and split equal states:
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

// What a draw's shader was given, read from the D3D device's constant shadow
// after the draw: the constants and option word of its material pass.
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
    // the pass's material (rb3-xenon rndobj/BaseMaterial.h); kDefaultMaterial
    // for a mesh's pass without one (NoMaterial)
    uint32_t mat;
    // +0xac: the material of the pass after it, which is a draw of its own
    // (the next one, in captures since passes were; before, the pass after
    // was folded into this draw, its constants these)
    uint32_t next_pass;
    uint8_t use_environ;  // +0x99: lit at all
    uint8_t intensify;
    uint8_t per_pixel_lit;
    // +0x9c: it writes alpha into the back buffer whatever its shader (the
    // back buffer's alpha is otherwise written by PSEUDO_HDR shaders alone);
    // 0 in captures from before
    uint8_t alpha_write;
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
    // format isn't decoded. s5 (kMapProjected), s1 (kMapNormal) or s14
    // (kMapDetailNormal) bound to a texture RB3 draws at runtime (its fetch
    // constant's base is one a texture pass draws: the shadow map, NgLight's
    // shadow, a character's head's normal map) is that texture's identity
    // and version instead (Texture::tex_obj), with guest memory's pixels as a
    // diffuse render target has them.
    std::shared_ptr<const Texture> maps[kNumShadeMaps];
    // the samplers its fetch constants describe (fetch_diffuse's for the
    // diffuse texture, fetch's for each map), which the renderers sample
    // them with; the default (TexSampler::filtered 0) where none was bound
    // and in captures from before they were kept
    TexSampler diffuse_sampler;
    TexSampler samplers[kNumShadeMaps];
};

// one draw: a material pass of a DxMesh::DrawShowing (RB3 draws the mesh's
// faces once for its material and once for each RndMat::NextPass after it,
// each with that pass's material, constants and cull), one DxMultiMesh
// instance in one pass, a particle system's quads, or a DxRnd::DrawRect quad.
// Captures from before passes were have one draw per mesh, with the first
// pass's material and the last pass's constants.
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
    // A DxRnd::DrawRect quad: its ShaderType (6 colour fills, 3 mip
    // downsample, 1 blur, 4 DOF, 11 movie, 16 the post copy...), -1 for a mesh
    // draw. Its geometry is the quad in clip space (z 0, w 1, uv 0..1 from the
    // top left through the material's texture transform as DrawRect applies
    // it, the vertex colour DrawRect gives it) with world and view_proj
    // identity, and rect the rectangle in the target's pixels (x, y, w, h).
    int32_t rect_shader = -1;
    float rect[4] = {};
    // a FinishDrawTarget mip downsample: the level it makes (1, 2...) from the
    // one before, in its pass's texture; it samples nothing else, so a renderer
    // can build the mips itself instead. 0 for any other draw.
    int32_t mip_level = 0;
    // Its cull mode, as the device had it after the draw: the low bits of
    // Xenos' PA_SU_SC_MODE_CNTL (kCullFront, kCullBack, kCullFrontIsCw;
    // D3DCULL_CW is 2, D3DCULL_CCW 6). RndMat's cull flag sets D3DCULL_CW and
    // RndShader::CheckForceCull overrides it (CCW for reflections, which
    // mirror). Outlines are drawn with it: a cel-shaded character's slightly
    // larger copy with its faces turned inward, whose near side it culls. 0
    // (none) for draws whose geometry band3 builds (particles, DrawRect
    // quads), whose winding isn't the game's, and in captures from before.
    uint8_t cull = 0;
    // TheRnd's draw mode when it drew (kDrawMode*): 0 in captures from before
    // it, which kept the colour passes' alone
    uint8_t draw_mode = 0;
};

inline constexpr uint8_t kCullFront = 1, kCullBack = 2, kCullFrontIsCw = 4;
// whether a draw with this cull mode drops a triangle whose corners go
// clockwise on the screen (y down) or, `clockwise` false, counter-clockwise
inline bool Culls(uint8_t cull, bool clockwise) {
    const bool front = clockwise == ((cull & kCullFrontIsCw) != 0);
    return (cull & (front ? kCullFront : kCullBack)) != 0;
}

// DxRnd::FinishPostProcess and CopyPostProcess's DrawRect ShaderType: the
// post-processed picture copied to the screen, which post_model.h redoes
inline constexpr int32_t kRectShaderPostCopy = 16;

// A Bink movie's frame: Movie::Impl::Draw (rb3-xenon movie/Movie.cpp) gives
// its material the frame's three planes, Y as the diffuse texture (s0), cR as
// the specular map (s2) and cB as the emissive map (s3), each 8 bits, and
// draws a DrawRect quad with ShaderType 11 (kMovieShader), into a TexMovie's
// texture (from Rnd::DrawPreClear) or the back buffer (the intro's
// MoviePanel). Its shade state keeps the three fetch constants and the two
// chroma planes as kMapSpecular and kMapGlow, whatever the option word (0);
// the shading turns them to RGB (shaders/shade.hlsli's MovieRgb).
inline constexpr int32_t kMovieShader = 11;
inline bool IsMovie(const DrawItem& d) { return d.rect_shader == kMovieShader; }

// What the renderers draw into the back buffer: its mesh draws and its
// DrawRect quads (flares, RndScreenMask's, the movie), but the post copy;
// texture passes are drawn too, the quads in them as well (soft_raster.h's
// PlanPasses).
inline bool DrawnToBackBuffer(const DrawItem& d) {
    return d.target == 0 && d.rect_shader != kRectShaderPostCopy;
}

// RndTex::Type of RndShadowMap's texture, the character self-shadow's 512x512
// depth (out/research/m3_render_targets.md 1.6): RndShadowMap::PrepShadow
// selects its light camera, which clears its depth to 1 (no colour), draws
// the character in draw mode 1 (shader kShadowmapShader, SKINNED alone, cull
// D3DCULL_CCW: back faces) and selects the world camera again, whose
// RndCam::Select resolves the depth into it; the character's SHADOW_BUFFER
// draws read it as s5 right after (ShadowMapOf)
inline constexpr uint32_t kTexTypeShadowMap = 0x42;

// RB3's draw modes (TheRnd's), as DrawItem::draw_mode keeps them: 0 the colour
// pass, 1 a shadow map's depth (RndShadowMap::PrepShadow), 3 NgLight's shadow
// casters into its own texture (NgLight::RenderShadows, no camera: its draws'
// view_proj is the VS's c4..c7, which it uploads itself; their shader has no
// lights and no DIFFUSE_MAP: white silhouettes), 6 the soft particles
// (IsSoftParticle), 7 a reflection's mirrored scene. rb3-xenon numbers those
// from NgLight's on one higher than retail does.
inline constexpr uint8_t kDrawModeNormal = 0;
inline constexpr uint8_t kDrawModeShadowDepth = 1;
inline constexpr uint8_t kDrawModeShadowCasters = 3;

// RndTex::Type of NgSpotlightDrawer's targets: the depth volume its cones
// add up in (640x360) and the density map its fog proxy's particles draw
// (320x180), both drawn after post-processing starts, for the composite
inline constexpr uint32_t kTexTypeDepthVolume = 0xA2;
inline constexpr uint32_t kTexTypeDensityMap = 0x122;

// A spotlight's cone: NgSpotlightDrawer::RenderConeDefs draws the beam's
// proxy mesh (no material) with ShaderType 2, kDepthVolumeShader, into the
// depth volume, adding up the light along the view ray inside the cone
// (shaders/spot_model.hlsli). The capture keeps it as a mesh draw with blend
// Add, no depth, its cull mode, tex the cross-section texture (s11) if the
// shader samples it, and the cone's numbers in its shade state's spotlight
// registers.
inline constexpr int32_t kDepthVolumeShader = 2;
inline bool IsSpotCone(const DrawItem& d, const ShadeInputs* s) {
    return s && s->shader_type == kDepthVolumeShader && d.rect_shader < 0;
}

// A mesh's material pass without a material (RndShader::SelectConfig(null):
// a mesh with none), which RB3 draws with TheRnd's default material (white,
// prelit, unlit, opaque; every RndShader's Select takes it for null): kept
// with that material's colour, blend and constants, its shade state's mat
// kDefaultMaterial (scene_capture.cpp's MeshParts). Captures from before kept
// none.
inline constexpr uint32_t kDefaultMaterial = 0xffffffffu;
inline bool NoMaterial(const DrawItem& d, const ShadeInputs* s) {
    return s && s->mat == kDefaultMaterial && d.rect_shader < 0;
}

// A soft particle: RndSoftParticleBuffer::DoPost draws the particle systems
// RndSoftParticles queued during the world's draws into its 320x180 surface
// (PostConsts::soft_surface), after post-processing starts, through the
// usual DxParticleSys::DrawParticles but in draw mode 6, where
// RndShaderParticles::CalcShaderOpts adds option bit 45. That shader fades
// the particle's alpha by how far in front of the scene's depth it is
// (shaders/shade.hlsli's SoftFade), reading the camera's range from PS c89,
// which its shade state keeps. The buffer is blurred and the composite adds
// it (post_model.h).
inline constexpr int32_t kParticleShader = 14;
inline bool IsSoftParticle(const DrawItem& d, const ShadeInputs* s) {
    return s && s->shader_type == kParticleShader && s->Option(shader_opt::kSoftParticles) &&
           d.rect_shader < 0;
}

// The shadow map a SHADOW_BUFFER draw reads (s5, kTexTypeShadowMap) as the
// capture kept it, its identity and version, or null: none, a capture from
// before (s5 was then guest memory's k_24_8, not decoded), or not one
inline const Texture* ShadowMapOf(const ShadeState* s) {
    if (!s || !s->Option(shader_opt::kShadowBuffer)) return nullptr;
    const Texture* t = s->maps[kMapProjected].get();
    return t && t->tex_obj && t->tex_type == kTexTypeShadowMap ? t : nullptr;
}

// The projected light's map (s5 of a NUM_PROJ draw lit per pixel: the
// renderers leave a vertex-lit one's out) when it's a texture RB3 draws, as
// the capture kept it (its identity and version), or null: NgLight's shadow,
// which NgLight::RenderShadows draws each frame into its 256x256 rendered-noz
// texture from the shadow casters (draw mode 3) and blurs twice in place
// (soft_raster.h's ShadowCasterPass, spot::SpotBlur)
inline const Texture* ProjectedTargetOf(const ShadeState* s) {
    if (!s || !s->OptionBits(shader_opt::kNumProj, 2) || !s->Option(shader_opt::kPerPixel))
        return nullptr;
    const Texture* t = s->maps[kMapProjected].get();
    return t && t->tex_obj && IsPassTargetType(t->tex_type) && t->tex_type != kTexTypeShadowMap
               ? t
               : nullptr;
}

// The normal map or the detail map (kMapNormal, kMapDetailNormal) when it's a
// texture RB3 draws, as the capture kept it (its identity and version), or
// null: a character's head's normal map, head_wrinkle_output.tex, which a
// texture pass composes from its expressions' as the face moves (guest
// memory's copy is garbage without --readback_resolve=full)
inline const Texture* MapTargetOf(const ShadeState* s, int map) {
    if (!s || (map != kMapNormal && map != kMapDetailNormal)) return nullptr;
    const Texture* t = s->maps[map].get();
    return t && t->tex_obj && IsPassTargetType(t->tex_type) && t->tex_type != kTexTypeShadowMap
               ? t
               : nullptr;
}

// A stretch of FrameCapture::draws that went to one target: the back buffer,
// or a texture between DxTex::MakeDrawTarget and FinishDrawTarget (a texture
// pass), which FinishDrawTarget resolves into the texture as a new version.
// Passes don't nest in RB3, so a frame's are one after another.
struct Pass {
    uint32_t tex_obj = 0;  // the DxTex drawn into; 0 the back buffer
    uint32_t first_draw = 0;
    uint32_t draw_count = 0;
    // the texture's size, type, mip count and D3DFORMAT (tex+0x4c, +0x50,
    // +0x48, +0x64, +0x74); 0 for the back buffer
    uint32_t width = 0, height = 0, tex_type = 0, num_mips = 0, format = 0;
    // what DxCam::Select cleared it to, as D3DCLEAR bits (0x0f colour, 0x30
    // depth and stencil), 0 if no camera did (those that bind it themselves
    // clear it their own way, or not at all)
    uint32_t clear_flags = 0;
    uint32_t clear_color = 0;  // D3DCOLOR, ARGB
    float clear_z = 0;
    // x, y, w, h in the target's pixels: the camera's screen rect times its
    // size; w 0 when no camera set one
    float viewport[4] = {};
    uint32_t cam = 0;  // the camera that selected it, 0 none
    uint32_t version = 0;  // the texture's version this pass made
    // the game frame it was drawn in (Present count): the capture's own, or an
    // earlier one for a pass carried in because the capture samples its output
    uint64_t from_frame = 0;
    std::string name;  // the texture's name (Hmx::Object), often empty
};

// A camera that drew into the back buffer, as DxCam::SetViewport set the
// device up for it when RndCam::Select selected it (rb3-xenon rnddx9/Cam.cpp,
// Rnd.cpp): its screen rect (cam+0x2cc) clamped to 0..1, so a two-player
// track camera's (+0.22, 0, 1, 1) is x 0.22 w 0.78, times the back buffer's
// size, each truncated to whole pixels as the viewport's are; and its z range
// (mZRange, cam+0x2c4), the viewport's MinZ and MaxZ. RB3 doesn't clear depth
// between the back buffer's cameras (only DxRnd::DoPostProcess does, for the
// overlay): it layers them by these, its device's depth 1 - (MinZ + z/w (MaxZ
// - MinZ)) with reverse Z (DxRnd::SetViewport flips MinZ and MaxZ, the test
// is GREATER and the clear 0), z/w the projection's 0 at the near plane to 1
// at the far one (soft_raster.h's LayoutBackBuffer).
struct CameraView {
    uint32_t cam = 0;
    float viewport[4] = {};  // x, y, w, h in the back buffer's pixels
    uint32_t target_w = 0, target_h = 0;  // the back buffer's size
    float zrange[2] = {0, 1};
};
static_assert(sizeof(CameraView) == 36, "CameraView has no padding: a capture saves its bytes");

struct FrameCapture {
    uint64_t frame = 0;
    uint64_t game_frame = 0;  // Present calls before this frame's
    std::vector<DrawItem> draws;
    std::vector<ShadeState> shades;  // the draws' distinct ones
    // the draws' passes in the order they were drawn, carried ones first; none
    // in a capture from before them (all its draws are the back buffer's)
    std::vector<Pass> passes;
    // DxRnd::DoPostProcess: the first draw after post-processing started
    // (kNoPost if it didn't this frame), and TheRnd's ProcCommands then (1
    // world, 2 post, 7 all; even/odd rendering alternates 1 and 2)
    static constexpr uint32_t kNoPost = ~0u;
    uint32_t post_boundary = kNoPost;
    uint32_t proc_cmds = 0;
    // what post-processing was set to do (read at DoPostProcess, valid 0 if
    // it didn't run or the capture is from before), and the constants RB3's
    // composite drew with (on frames that post-process); a composed frame
    // has its post frame's, which post-processes the world it shows
    PostParams post;
    PostConsts post_consts;
    // The noise map the composite's film grain reads (PostParams::noise_map's
    // pixels and mips) and the sampler it reads it with: on a post frame
    // sampler 13's, PostConsts::noise_fetch, as NgPostProc::CheckNoise bound
    // it; on a world frame (even/odd rendering) the last post frame's, for
    // the grain the next frame gives its world. Null where the noise is off,
    // the format isn't decoded, and in captures from before.
    std::shared_ptr<const Texture> noise_map;
    TexSampler noise_sampler;
    // With even/odd rendering a frame that draws no world (proc_cmds 2)
    // presents the last one that did, so its capture has that frame's world
    // in front of its own draws from post_boundary on (frame_compose.h):
    // composed 1, and world_frame that frame's game_frame. Otherwise 0, and
    // the world is the frame's own: world_frame is its game_frame.
    uint32_t composed = 0;
    uint64_t world_frame = 0;
    // the display gamma ramp the presenter applied to the game's picture of
    // it (gamma_ramp.h), read at the frame's end; kNone in captures from
    // before it, and where it couldn't be read
    GammaRamp gamma;
    uint32_t cams = 0;             // camera selects that drew to the back buffer
    uint32_t skipped_target = 0;   // draws for a camera with a target, but no texture pass open
    uint32_t skipped_velocity = 0; // motion blur velocity pass
    // draws in a shadow's draw mode (1, 3) outside its pass: none expected
    // (Target keeps 1 in a shadow map's pass, 3 in any texture pass)
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
    // texture passes: drawn this frame, carried in from earlier frames, and
    // left out for drawing nothing the capture keeps (the velocity buffer's:
    // draw mode 5 isn't recorded)
    uint32_t passes_own = 0;
    uint32_t passes_carried = 0;
    uint32_t passes_empty = 0;
    // diffuse textures that are pass targets (and s5 kept as one: the shadow
    // map, NgLight's), by (texture, version) sampled: made by a pass in the
    // capture (own or carried), by a pass left out above because all its
    // draws were too (rt_filtered: for their draw mode, as the velocity
    // buffer's are, or for having no material or geometry the capture
    // draws), or
    // by none it has (rt_missing: recorded in a frame whose passes weren't
    // kept, made before band3 saw it, or drawn by something band3 doesn't
    // record), so rt_missing 0 means none is missing that the capture could
    // have had
    uint32_t rt_sampled = 0;
    uint32_t rt_missing = 0;
    uint32_t rt_filtered = 0;
    // the rt_filtered ones, as texture << 32 | version, for counting them
    // again over a composed frame (frame_compose.h) and for replay's --list
    std::vector<uint64_t> rt_filtered_keys;
    // back-buffer snapshots and device textures sampled (refraction's
    // pre-process buffer), which no pass makes
    uint32_t rt_snapshots = 0;
    // passes that didn't pair up (a Make while one was open, a Finish of
    // another texture, a frame ending inside one): dropped
    uint32_t passes_unbalanced = 0;
    // material passes: a mesh's second pass or a later one (RndMat::NextPass;
    // a multimesh's counted once for all its instances), and passes drawn
    // with no material (RndShader::SelectConfig(null): a mesh without one),
    // the spotlights' cones apart: kept, with TheRnd's default material
    // (NoMaterial), where they have geometry (in captures from before, left
    // out and counted in skipped_no_geom too)
    uint32_t later_passes = 0;
    uint32_t skipped_no_mat = 0;
    // DxMesh::DrawFaces calls outside a DxMesh::DrawShowing, not recorded:
    // RndTexBlender's, into its textures (the velocity buffer's aren't counted)
    uint32_t faces_elsewhere = 0;
    // TheRnd's clear colour (+0x2c, r g b a), what DxRnd::BeginDrawing clears
    // the back buffer to, read at the frame's end; has_clear_color 0 in
    // captures from before it, which the renderers clear to 0xff202020
    uint32_t has_clear_color = 0;
    float clear_color[4] = {};
    // the back-buffer cameras the frame selected (CameraView), each once;
    // none in captures from before them, whose renderers clear depth for
    // each camera instead (RasterOptions::clear_depth_per_camera)
    std::vector<CameraView> cameras;
};

// the back-buffer camera `cam`'s view in fc.cameras, or null
inline const CameraView* CameraOf(const FrameCapture& fc, uint32_t cam) {
    for (const CameraView& c : fc.cameras)
        if (c.cam == cam) return &c;
    return nullptr;
}

// What the texture-pass recording has done and cost since the game started,
// while capture was off (its always-on part, native_view_record_targets, `on`
// now): passes the game drew, those it
// recorded (the rest were into textures drawn regularly, every frame or every
// other, which a capture draws again itself), their draws, and the game
// thread's time in that recording (the hooks of passes it skips aren't timed:
// a lookup each).
struct PassRecordingStats {
    bool on = false;
    uint64_t passes = 0;
    uint64_t passes_recorded = 0;
    uint64_t draws_recorded = 0;
    double ms = 0;
};
PassRecordingStats GetPassRecordingStats();

// What capture costs the game's render thread, since the game started: each
// kind of hook's time past its early-out (taking g_state_mutex aside), always
// counted while the hooks work (capture on, or texture passes recorded); and
// with native_view_capture_profile (Band3/Debug, off by default) the steps
// inside them, each the time since the step before it ended, so they add up
// to the hooks' time (rest: what no step names), at a clock read each. The
// harness's `native_view stats` reports it per game frame (its `capture`).
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
        kStepGeomMiss,      // and decoded (bytes: the Geometry's)
        kStepGeomMutable,   // a mutable mesh's CPU verts, decoded each draw
        kStepParticleGeom,  // a particle system's quads
        kStepRectGeom,      // a DrawRect's quad
        kStepItem,          // the material's fields, the view-projection
        kStepTexLookup,     // a loaded texture's key hashed and found
        kStepTexDecode,     // and decoded (bytes: its levels'), or a movie's
                            // plane copied to decode later (bytes: those)
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
    // draws added (to frames and recorded passes alike), shade states kept
    // new, the shared objects made (frames, geometry, textures, passes), and
    // what the steps above count
    uint64_t draws = 0;
    uint64_t new_shades = 0;
    uint64_t allocs = 0;
    uint64_t geom_miss_bytes = 0;
    uint64_t tex_decode_bytes = 0;
    uint64_t bones = 0;
    // movie planes decoded off the game's thread (Texture::deferred), by
    // whoever took the capture first (the native renderer's worker, a
    // harness capture), and the microseconds that took them: not the game's
    uint64_t deferred_decodes = 0;
    uint64_t deferred_decode_us = 0;
    // the caches' sizes now: render targets known, geometry, loaded textures
    // and maps
    uint64_t rts = 0, geoms = 0, texs = 0, map_texs = 0;
};
CaptureProfile GetCaptureProfile();
// what `now` counted since `before` (the sizes and steps_on are now's)
CaptureProfile CaptureProfileSince(const CaptureProfile& now, const CaptureProfile& before);

// capture costs a little every frame, so it only runs while something wants
// it: each Acquire is matched by a Release
void AcquireCapture();
void ReleaseCapture();

// For a render check: waits for the next frame captured whose world is the one
// the game's picture of it shows (frame_compose.h's PresentsCapturedWorld:
// with even/odd rendering, a post frame composed with the world frame before
// it; without, any whole frame; for frames that don't say, as some menus'
// don't, one that draws about as much as the couple of frames before it),
// holds the game at the end of it, waits
// `settle` for the emulated GPU to show it, runs `while_held` (a screenshot of
// the same frame) and lets the game go on. The game is never held more than
// three seconds. Null, without running while_held, if no frame came in time.
// If none of the 30 frames after the request was such a frame, it takes the
// last of them all the same, and says so in `fell_back`.
std::shared_ptr<const FrameCapture> CaptureHeldFrame(
    const std::function<void()>& while_held, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle = std::chrono::milliseconds(150),
    bool* fell_back = nullptr);

// the latest complete frame, composed with the world before it if it drew
// none (frame_compose.h), or null before the first. Its movie planes
// (Texture::deferred) are decoded first, on the caller's thread, if no one
// has yet, as CaptureHeldFrame's are once the game goes on.
std::shared_ptr<const FrameCapture> LatestCapture();
// and when it was published, at the end of the game's DxRnd::Present: where
// the native renderer's latency starts
std::shared_ptr<const FrameCapture> LatestCapture(
    std::chrono::steady_clock::time_point& published);

// A number that moves on with each capture published and each
// WakeCaptureWaiters, so the native renderer's worker can sleep until there
// is something to draw: WaitForCapture waits up to `timeout` for it to move
// past `epoch` and returns it as it is then.
uint64_t CaptureEpoch();
uint64_t WaitForCapture(uint64_t epoch, std::chrono::milliseconds timeout);
// wakes WaitForCapture as a capture would (the worker has another reason to draw)
void WakeCaptureWaiters();

// The game's frames: when each of the newest few thousand DxRnd::Presents
// ended (captured or not), from `since` on, oldest first
std::vector<std::chrono::steady_clock::time_point> GamePresentTimes(
    std::chrono::steady_clock::time_point since);

// native_view_rt_fallback: whether render targets' textures carry the pixels
// guest memory holds too ("guest", the default) or only their identity
// ("none"). Never while renderer is native: the emulated GPU skips the draws
// that would make them (gpu_skip.h), so they're stale by construction, and
// the native renderer draws them from their passes, by identity and version.
bool RtFallbackGuest();

}  // namespace band3::render
