#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
// early-out each.
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
};

struct Geometry {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
};

// RndTex::Type (tex+0x48) values that make a texture's pixels something RB3
// draws at runtime rather than loads: kRendered and what's built on it
// (0x22 NoZ, 0x42 shadow map, 0xA2, 0x122), back-buffer snapshots (8, 0x18)
// and device textures (0x1000, DxRnd's pre and post buffers)
inline bool IsRenderedType(uint32_t type) {
    return (type & 2) || (type & 8) || type == 0x1000;
}
// of those, the ones texture passes draw into
inline bool IsPassTargetType(uint32_t type) { return (type & 2) != 0; }

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
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

// one draw (first material pass): a DxMesh::DrawShowing, one DxMultiMesh
// instance, a particle system's quads, or a DxRnd::DrawRect quad
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
    // top left, the vertex colour DrawRect gives it) with world and view_proj
    // identity, and rect the rectangle in the target's pixels (x, y, w, h).
    int32_t rect_shader = -1;
    float rect[4] = {};
    // a FinishDrawTarget mip downsample: the level it makes (1, 2...) from the
    // one before, in its pass's texture; it samples nothing else, so a renderer
    // can build the mips itself instead. 0 for any other draw.
    int32_t mip_level = 0;
};

// What the renderers draw of a capture, for now: the back buffer's mesh
// draws. Texture passes and DrawRect quads are recorded for what comes next.
inline bool DrawnToBackBuffer(const DrawItem& d) { return d.target == 0 && d.rect_shader < 0; }

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
    uint32_t cams = 0;             // camera selects that drew to the back buffer
    uint32_t skipped_target = 0;   // draws for a camera with a target, but no texture pass open
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
    // texture passes: drawn this frame, carried in from earlier frames, and
    // left out for drawing nothing the capture keeps (shadow maps: draw modes
    // 1 and 3 aren't recorded)
    uint32_t passes_own = 0;
    uint32_t passes_carried = 0;
    uint32_t passes_empty = 0;
    // diffuse textures that are pass targets, by (texture, version) sampled:
    // made by a pass in the capture (own or carried), or by none it has
    // (rt_missing: recorded in a frame whose passes weren't kept, made before
    // band3 saw it, or a pass left out above)
    uint32_t rt_sampled = 0;
    uint32_t rt_missing = 0;
    // back-buffer snapshots and device textures sampled (refraction's
    // pre-process buffer), which no pass makes
    uint32_t rt_snapshots = 0;
    // passes that didn't pair up (a Make while one was open, a Finish of
    // another texture, a frame ending inside one): dropped
    uint32_t passes_unbalanced = 0;
};

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

// native_view_rt_fallback: whether render targets' textures carry the pixels
// guest memory holds too ("guest", the default) or only their identity ("none")
bool RtFallbackGuest();

}  // namespace band3::render
