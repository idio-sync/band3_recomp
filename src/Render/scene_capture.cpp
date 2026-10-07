#include "src/Render/scene_capture.h"

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "generated/band3_init.h"
#include "src/Hooks/aspect.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Render/deferred_decode.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/guest_formats.h"
#include "src/Render/present_model.h"
#include "src/Render/renderer_mode.h"
#include "src/Render/renderer_switch.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/Render/sync_gpu/sync_graphics_system.h"
#include "src/settings.h"
#include "src/stall_watch.h"

// See scene_capture.h.

extern "C" void __imp__RndCam__Select(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxMesh__DrawShowing(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxMesh__DrawFaces(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndShader__SelectConfig(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__Present(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxMultiMesh__DrawShowing(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxParticleSys__DrawParticles(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndShader__Cache(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__DrawRect_82733538(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__MakeDrawTarget(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__DoPostProcess(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndTex__dt(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__FinishPostProcess(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__NgDOFProc__DoPost(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Bloom_Blur(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxTex__MakeDrawTarget(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxTex__FinishDrawTarget(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxCam__Select(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxTex__SyncBitmap(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__NgSpotlightDrawer__BlurRT_824D24D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndSoftParticleBuffer__DoPost(PPCContext& ctx, uint8_t* base);

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
// (guest_formats.h's DecodePacked)
constexpr uint32_t kPackedVert_Size = guest_format::kPackedVertSize;
// RndMat (rndobj/BaseMaterial.h)
constexpr uint32_t kMat_Blend = 0x28;
constexpr uint32_t kMat_Color = 0x2c;
constexpr uint32_t kMat_ZMode = 0x3c;
constexpr uint32_t kMat_TexGen = 0x44;
constexpr uint32_t kMat_TexXfm = 0x4c;  // Transform: Matrix3 rows of 16 bytes, then v at +0x30
constexpr uint32_t kTexGenXfmOrigin = 4;
constexpr uint32_t kMat_DiffuseTex = 0x8c + 8;
constexpr uint32_t kMat_Intensify = 0x98;
constexpr uint32_t kMat_UseEnviron = 0x99;
constexpr uint32_t kMat_Prelit = 0x9a;
constexpr uint32_t kMat_AlphaCut = 0x9b;
constexpr uint32_t kMat_AlphaWrite = 0x9c;
constexpr uint32_t kMat_AlphaThreshold = 0xa0;
constexpr uint32_t kMat_NextPass = 0xa4 + 8;
constexpr uint32_t kMat_Fur = 0x104 + 8;
constexpr uint32_t kMat_ShaderVariation = 0x118;
constexpr uint32_t kMat_PerPixelLit = 0x11d;
// each ShadeMap's ObjPtr in the material (pointer at +8), 0 for the
// environment's (projected light, shadow buffer)
constexpr uint32_t kMat_Map[kNumShadeMaps] = {0xd4 + 8,  0xec + 8, 0xe0 + 8, 0xf8 + 8,
                                              0,         0,        0x164 + 8, 0x148 + 8};
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
constexpr uint32_t kParticle_Angle = 0x50;
constexpr uint32_t kParticle_SwingArm = 0x54;
constexpr uint32_t kParticle_Next = 0x5c;
constexpr uint32_t kMaxParticles = 16000;  // four verts each, u16 indices
// Hmx::Object's name (obj/Object.h)
constexpr uint32_t kObj_Name = 0x18;
constexpr uint32_t kMaxName = 64;
// RndTex and DxTex (rndobj/Tex.h, rnddx9/Tex.h; out/research/m3_render_targets.md)
constexpr uint32_t kTex_Type = 0x48;
constexpr uint32_t kTex_Width = 0x4c;
constexpr uint32_t kTex_Height = 0x50;
constexpr uint32_t kTex_NumMips = 0x64;
constexpr uint32_t kDxTex_Format = 0x74;
constexpr uint32_t kDxTex_Texture = 0x78;
constexpr uint32_t kTexType_NoZ = 0x20;
// RndCam (rndobj/Cam.h)
constexpr uint32_t kCam_ScreenRect = 0x2cc;  // Hmx::Rect, 0..1 of the target
constexpr uint32_t kCam_TargetTex = 0x2dc + 8;
constexpr uint32_t kCam_ViewProj = 0x2ec;  // Hmx::Matrix4, written by DxCam::Select
// the depths DxCam::Select clears a target to, from the image's constants:
// shadow maps' (1) and the rest's (0, reverse Z)
constexpr uint32_t kClearDepthShadow = 0x820009FC;
constexpr uint32_t kClearDepth = 0x82000D78;
// TheRnd: the screen's size (Rnd::DrawRectScreen scales by it) and the frame's
// ProcCommands (Rnd::BeginDrawing copies ProcCounter's there)
constexpr uint32_t kRnd_Width = 0x3c;
constexpr uint32_t kRnd_Height = 0x40;
constexpr uint32_t kRnd_ProcCmds = 0x16c;
// its clear colour, four floats r g b a, which DxRnd::BeginDrawing clears the
// back buffer to (out/research/n1_runtime_survey.md 3)
constexpr uint32_t kRnd_ClearColor = 0x2c;
// its default material (mDefaultMat, rndobj/Rnd.h: white, prelit, unlit),
// which RndShader's Select draws a pass without a material with
constexpr uint32_t kRnd_DefaultMat = 0x94;
// XDK D3D resources (rb3-xenon xdk/d3d9i/d3d9.h)
constexpr uint32_t kD3DVertexBuffer_Fetch = 0x18;
constexpr uint32_t kD3DIndexBuffer_Address = 0x18;
constexpr uint32_t kD3DIndexBuffer_Size = 0x1c;
constexpr uint32_t kD3DBaseTexture_Fetch = 0x1c;
// TheRnd's draw mode, which a pass sets while it draws. Retail sets 1
// (RndShadowMap::PrepShadow, into the shadow map), 3 (NgLight::RenderShadows:
// shadow casters into its shadow texture, which it makes the draw target itself,
// with no camera, so the camera still says back buffer), 5
// (RndVelocityBuffer::Draw; DxMesh::DrawShowing draws through it), 6
// (RndSoftParticleBuffer::DoPost: its queued particles into its surface) and
// 7 (WorldReflection::DrawShowing, the mirrored scene through a copy of the
// current camera, so to the back buffer).
// rb3-xenon's Rnd::Mode numbers NgLight's (kDrawOcclusion) and those after it
// one higher than retail does. scene_capture.h has 0, 1 and 3.
constexpr uint32_t kDrawModeHolder = 0x82C76B68;  // TheRnd*
constexpr uint32_t kDrawMode = 0xfc;
constexpr uint32_t kDrawModeVelocity = 5;
constexpr uint32_t kDrawModeSoftParticles = 6;
constexpr uint32_t kDrawModeReflection = 7;
// the D3D device (TheDxRnd + 0x1c4) and its constant shadow, which
// DxShaderMgr::SetVConstant/SetPConstant and SetTexture write
// (rb3-xenon xdk/d3d9i/d3d9.h D3DDevice::m_Constants)
constexpr uint32_t kD3DDeviceHolder = 0x82E04CFC;
constexpr uint32_t kDev_TextureFetch = 0x480;  // 26 of 24 bytes
constexpr uint32_t kDev_VertexShaderF = 0x780;
// the view-projection DxCam::Select uploads (kVS_ViewProjMatrix), which
// NgLight::SetShadowTransforms sets itself for its camera-less pass: VS
// c4..c7, the matrix's columns
constexpr uint32_t kVsViewProj = 4;
constexpr uint32_t kDev_PixelShaderF = 0x1780;
// its PA_SU_SC_MODE_CNTL, whose low bits RndRenderState::SetCullMode sets
// (the XDK's D3DDevice_SetRenderState_CullMode, sub_828502B8)
constexpr uint32_t kDev_ModeCntl = 0x2948;
constexpr uint32_t kEnvironCurrent = 0x82CC0280;  // RndEnviron::sCurrent
// post-processing (out/research/m4_postproc.md 1, 2 and 6): TheRnd's
// mDisablePostProc, its mPostProcOverride (a PostProcessor, which is the
// proc + 0x28) and its copy of the world camera (a pointer)
constexpr uint32_t kRnd_DisablePostProc = 0x105;
constexpr uint32_t kRnd_PostProcOverride = 0x124;
constexpr uint32_t kRnd_WorldCam = 0xa4;
constexpr uint32_t kPostProcessor = 0x28;
constexpr uint32_t kPostProcCurrent = 0x82CC27F0;  // RndPostProc::sCurrent
constexpr uint32_t kPostProc_BloomColor = 0x30;
constexpr uint32_t kPostProc_BloomThreshold = 0x40;
constexpr uint32_t kPostProc_BloomIntensity = 0x44;
constexpr uint32_t kPostProc_BloomGlare = 0x48;
constexpr uint32_t kPostProc_BloomStreak = 0x49;
constexpr uint32_t kPostProc_Hue = 0x64;  // then saturation, lightness, contrast, brightness
constexpr uint32_t kPostProc_LevelInLo = 0x78;
constexpr uint32_t kPostProc_LevelInHi = 0x88;
constexpr uint32_t kPostProc_LevelOutLo = 0x98;
constexpr uint32_t kPostProc_LevelOutHi = 0xa8;
constexpr uint32_t kPostProc_Xfm = 0xb8;  // rows 0x10 apart, then the translation
constexpr uint32_t kPostProc_ColorMod = 0x12c;
constexpr uint32_t kPostProc_EmulateFps = 0x168;
// the noise's (NgPostProc::CheckNoise; out/research/n1_post_noise.md 1):
// base scale x, y, top scale, intensity, stationary and midtone bytes, the
// map's RndTex, and NgPostProc's stationary seeds
constexpr uint32_t kPostProc_NoiseBase = 0x130;
constexpr uint32_t kPostProc_NoiseTop = 0x138;
constexpr uint32_t kPostProc_NoiseIntensity = 0x13c;
constexpr uint32_t kPostProc_NoiseStationary = 0x140;
constexpr uint32_t kPostProc_NoiseMidtone = 0x141;
constexpr uint32_t kPostProc_NoiseMap = 0x14c;
constexpr uint32_t kPostProc_NoiseSeeds = 0x20c;
// the trails' threshold and duration (RndPostProc::BlendPrevious)
constexpr uint32_t kPostProc_TrailThreshold = 0x150;
constexpr uint32_t kPostProc_TrailDuration = 0x154;
// the sampler CheckNoise binds the noise map to
constexpr uint32_t kNoiseSampler = 13;
constexpr uint32_t kDOFProcHolder = 0x82CC6368;  // TheDOFProc
constexpr uint32_t kDOF_Enabled = 0x2c;
constexpr uint32_t kDOF_Scale = 0x30;  // then bias, focal, blur depth, min and max blur
constexpr uint32_t kDOFOverride_BlurWidthScale = 0x82C70440 + 0x18;
constexpr uint32_t kCam_Near = 0x2b4;
constexpr uint32_t kCam_Far = 0x2b8;
constexpr uint32_t kCam_ZRange = 0x2c4;
// TheShaderMgr, whose flags at +0x25.. say what the composite does
constexpr uint32_t kShaderMgrHolder = 0x82C76CE0;
// NgSpotlightDrawer's shared resources (CheckRTs; out/research/
// spotlight_survey.md 1): a pointer to them, and the depth volume's DxTex
// at +8
constexpr uint32_t kSpotSharedHolder = 0x82CC77D4;
constexpr uint32_t kSpotShared_DepthVolume = 8;
// the sampler of the cone shader's cross-section texture, and the PS
// register whose x weighs it (0 or 1: 0 doesn't sample it)
constexpr uint32_t kSpotXsecSampler = 11;
constexpr int kSpotXsecWeightReg = 86;
// DxRnd::DrawRect's ShaderType for a blur
constexpr int32_t kRectShaderBlur = 1;
// RndSoftParticleBuffer::DoPost's r3 is the buffer's PostProcessor (+0x28),
// its two surfaces' DxTex at +4 and +8 (out/research/softparticle_survey.md 1)
constexpr uint32_t kSoftPost_Surfaces = 4;
// The camera motion blur's (out/research/n5_hub_soft.md 3):
// RndVelocityBuffer::sSingleton, its fields as rb3-xenon's
// rndobj/VelocityBuffer.h has them, checked against the retail Draw
// (0x82B855F0); the proc's mMotionBlurVelocity (DoVelocity's lbz 0x1A4);
// TheNgRnd (Draw's PreDepthTexture and DrawRectDepth calls go through it) and
// its pre-pass depth texture (DxRnd::PreDepthTexture: lwz 0x340); and the
// samplers the composite reads the scene and the velocity texture from
constexpr uint32_t kVelocityBuffer = 0x82E12BA0;
constexpr uint32_t kVel_ViewProj = 0x8;
constexpr uint32_t kVel_DepthRange = 0x48;
constexpr uint32_t kVel_FrustumNear = 0x58;
constexpr uint32_t kVel_FrustumCorners = 0x68;
constexpr uint32_t kVel_Cam = 0xa8;
constexpr uint32_t kVel_Scale = 0x36be8;
constexpr uint32_t kVel_Xfms = 0x36bec;  // two Hmx::Matrix4, 64 bytes apart
constexpr uint32_t kVel_Index = 0x36c6c;
constexpr uint32_t kVel_Frame = 0x36c70;
constexpr uint32_t kVel_Tex = 0x36c74;
constexpr uint32_t kVel_LastCam = 0x36c7c;
constexpr uint32_t kPostProc_MotionBlurVelocity = 0x1a4;
constexpr uint32_t kNgRndHolder = 0x82C76B6C;
constexpr uint32_t kNgRnd_PreDepth = 0x340;
constexpr uint32_t kSceneSampler = 6;
constexpr uint32_t kVelocitySampler = 10;
// its transform caches, RndXfmCache (VelocityBuffer.h, checked against the
// retail DrawMesh, 0x82B852A8): two, 0x1B584 apart from +0xAC, each its
// meshes by bone slot (+0), 12 floats a slot (+0x1F40) and the slots used
// (+0x1B580); and a mesh's slot in each (RndMesh's mMotionCache keys, +0x138
// + 4 * index); its material's z mode (kMat_ZMode) 2, transparent, draws none
constexpr uint32_t kVel_XfmCaches = 0xac;
constexpr uint32_t kXfmCache_Size = 0x1b584;
constexpr uint32_t kXfmCache_Floats = 0x1f40;
constexpr uint32_t kXfmCache_Used = 0x1b580;
constexpr uint32_t kMesh_MotionKeys = 0x138;

constexpr uint32_t kMaxBufferBytes = 64u << 20;
constexpr uint32_t kMaxTextureSize = 4096;

uint32_t Be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
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

// FNV-style mixing in four lanes, 32 bytes a round, which don't wait on each
// other's multiplies, so the CPU runs them side by side (about four times as
// fast as one lane on a shade state's 2.7 KB, and than byte-wise FNV on the
// keys' few hundred bytes), then folded together with the tail
uint64_t HashBytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    uint64_t lane[4] = {1469598103934665603ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full,
                        0x165667B19E3779F9ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        for (int l = 0; l < 4; l++) {
            uint64_t w;
            std::memcpy(&w, b + i + l * 8, 8);
            lane[l] = (lane[l] ^ w) * 1099511628211ull;
            lane[l] ^= lane[l] >> 29;
        }
    }
    uint64_t h = n;
    for (uint64_t l : lane) {
        h = (h ^ l) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
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
    // the tangent and its handedness, which FillCompressedVertex packs into
    // the vertex buffer's 2_10_10_10 (guest_formats.h's DecodePacked): w as
    // its two bits keep it, -1, 0 or 1
    for (int i = 0; i < 3; i++) v.tan[i] = g.F32(a + 0x50 + i * 4);
    v.tan[3] = std::clamp(std::round(g.F32(a + 0x5c)), -1.0f, 1.0f);
    return v;
}

// ---------------------------------------------------------------------------
// textures

// A 2D texture and its mip chain from its fetch constant (guest_formats.h's
// DecodeTextureLevels); empty rgba for formats not handled
std::shared_ptr<Texture> DecodeTexture(const Guest& g, const uint32_t f[6]) {
    auto tex = std::make_shared<Texture>();
    const guest_format::FetchLayout l = guest_format::ReadFetchLayout(f);
    const uint8_t* base = l.base_address ? GpuHost(g, l.base_address) : nullptr;
    const uint8_t* mips = l.mip_address && l.mip_max ? GpuHost(g, l.mip_address) : nullptr;
    if (!guest_format::DecodeTextureLevels(base, mips, f, *tex, kMaxTextureSize)) {
        tex->width = tex->height = 0;
        tex->rgba.clear();
        tex->mips.clear();
    }
    return tex;
}

// ---------------------------------------------------------------------------
// capture state, touched by the hooks under g_state_mutex: RB3 draws on more
// than one thread, though never on two at once (at boot its splash thread
// draws the loading movie, into a texture too, while the main thread loads
// and frees textures, and the main thread draws from the main menu on), and
// frees textures on whichever thread lets go of them

struct GeomEntry {
    uint64_t key;
    std::shared_ptr<const Geometry> geom;
};

struct TexEntry {
    uint64_t key;
    std::shared_ptr<const Texture> tex;  // empty rgba: not decoded
};

using ShadeIndex = std::unordered_multimap<uint64_t, int32_t>;  // by a hash of the inputs

// a texture pass's draws, recorded apart from any frame: sealed at
// FinishDrawTarget, then added to the frame being captured and kept as its
// texture's last pass, for later captures that sample it
struct PassRecord {
    Pass pass;
    FrameCapture content;  // its draws and their shades, and its counts
    // the render targets its draws sampled, as RtKey(tex, version)
    std::vector<uint64_t> samples;
};

uint64_t RtKey(uint32_t tex, uint32_t version) { return uint64_t(tex) << 32 | version; }

// what's known of a texture that passes draw into
struct RtState {
    uint32_t version = 0;  // its passes resolved since it was made
    // its texture's base address, physical (TexBase), as a fetch constant
    // that binds it has it: what tells s5 bound to it (CaptureShade)
    uint32_t base = 0;
    uint64_t made_frame = ~0ull;  // the game frame of its last pass
    // its passes in a row each within RepeatFrames() of the one before
    uint32_t repeats = 0;
    // its last two recorded passes (a texture drawn every frame is sampled
    // before it's drawn again too), null where the pass wasn't recorded or
    // drew nothing; one whose draws were all left out (AllLeftOut) is kept,
    // with none
    std::shared_ptr<const PassRecord> last, before;
    // the Texture a capture's draws sampled it as last, and the guest pixels
    // that has (native_view_rt_fallback guest)
    std::shared_ptr<const Texture> sampled, sampled_pixels;
};

// where a draw goes: the frame being captured, or the texture pass being
// recorded
struct Sink {
    FrameCapture& fc;
    ShadeIndex& shades;
    std::vector<uint64_t>& samples;
    uint32_t target;  // 0 the frame's back buffer
    uint8_t draw_mode = kDrawModeNormal;  // TheRnd's, for DrawItem::draw_mode
};

// the texture pass the game is drawing, between DxTex::MakeDrawTarget and
// FinishDrawTarget
struct OpenPass {
    uint32_t tex = 0;  // 0 none
    bool record = false;
    bool in_finish = false;  // FinishDrawTarget's mip downsamples
    int32_t mips_drawn = 0;
    std::shared_ptr<PassRecord> rec;
    ShadeIndex shades;
};

struct State {
    std::shared_ptr<FrameCapture> building = std::make_shared<FrameCapture>();
    uint32_t cam = 0;
    bool cam_backbuffer = false;
    bool cam_counted = false;
    bool vp_valid = false;
    Mat4 vp{};
    uint64_t frame = 0;
    uint64_t game_frame = 0;  // Present calls so far
    std::unordered_map<uint32_t, GeomEntry> geoms;
    std::unordered_map<uint32_t, TexEntry> texs;      // by D3D texture
    std::unordered_map<uint32_t, TexEntry> map_texs;  // by base address
    // the building frame's shades, the render targets it sampled, and those
    // its own passes made whose draws were all left out (AllLeftOut)
    ShadeIndex shades;
    std::vector<uint64_t> samples;
    std::unordered_set<uint64_t> left_out;
    OpenPass open;
    // by DxTex, every texture a pass has drawn into since it was made
    std::unordered_map<uint32_t, RtState> rts;
    // capture was on at the last frame's end, so this frame's is whole
    bool captured_last = false;
    // the last frame captured whole that drew the world, for the frames after
    // it that don't (frame_compose.h)
    std::shared_ptr<const FrameCapture> last_world;
    // while RndSoftParticleBuffer::DoPost runs: the surface it draws its
    // particles into (0 otherwise)
    uint32_t soft_surface = 0;
    // the last post frame's noise map and sampler (FrameCapture::noise_map),
    // for the world frames after it, and its base address
    std::shared_ptr<const Texture> noise_map;
    TexSampler noise_sampler;
    uint32_t noise_base = 0;
    // the meshes the last post frame drew into the velocity buffer with
    // their own motion (DxMesh::DrawShowing's mesh, its cull mode then) and
    // the game frame it was: a world frame's object pass is theirs
    // (EmulateVelocityObjects)
    struct VelocityMesh {
        uint32_t mesh = 0;
        uint8_t cull = 0;
    };
    std::vector<VelocityMesh> velocity_meshes;
    uint64_t velocity_meshes_frame = 0;
    // the frame's draws logged so far (DiagLog): passes without a material,
    // DrawFaces outside a DrawShowing
    uint32_t logged_no_mat = 0;
    uint32_t logged_elsewhere = 0;
    // what the hooks cost (GetCaptureProfile), and where the step running
    // started (Lap)
    CaptureProfile profile;
    std::chrono::steady_clock::time_point profile_mark;
    // the profile as the last frame ended, and the game's frame ending now
    // (from the Present before), for each frame's FrameCapture::cost
    CaptureProfile cost_mark;
    uint64_t game_ns = 0;
};

State& S() {
    static State state;
    return state;
}

// native_view_capture_profile, kept by its change callback: the hooks' steps
// are timed too (CaptureProfile)
std::atomic<bool> g_profile_steps{false};

uint64_t Nanos(std::chrono::steady_clock::duration d) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

// A step of the running hook's ends: its time since the step before ended
// (or the hook began) counts as `step`'s, with native_view_capture_profile
// on. Under g_state_mutex, as State is.
void Lap(State& s, CaptureProfile::Step step) {
    if (!g_profile_steps.load(std::memory_order_relaxed)) return;
    const auto now = std::chrono::steady_clock::now();
    s.profile.step_ns[step] += Nanos(now - s.profile_mark);
    s.profile.step_calls[step]++;
    s.profile_mark = now;
}

// A hook's work, timed into its kind's total (CaptureProfile::hook_ns), and
// what its steps didn't name into kStepRest. Made once g_state_mutex is held,
// so it's let go of before the lock is.
class HookTimer {
 public:
    explicit HookTimer(CaptureProfile::Hook hook)
        : hook_(hook), start_(std::chrono::steady_clock::now()) {
        S().profile_mark = start_;
    }
    ~HookTimer() {
        State& s = S();
        const auto end = std::chrono::steady_clock::now();
        s.profile.hook_ns[hook_] += Nanos(end - start_);
        s.profile.hook_calls[hook_]++;
        if (!g_profile_steps.load(std::memory_order_relaxed)) return;
        s.profile.step_ns[CaptureProfile::kStepRest] += Nanos(end - s.profile_mark);
        s.profile.step_calls[CaptureProfile::kStepRest]++;
    }
    HookTimer(const HookTimer&) = delete;
    HookTimer& operator=(const HookTimer&) = delete;

 private:
    CaptureProfile::Hook hook_;
    std::chrono::steady_clock::time_point start_;
};

// State's, taken by each hook once it's past its early-out, so a game that
// never uses the native view doesn't take it but at the frame's end; never
// held across a call into the game (a hook's __imp__, which can reach other
// hooks: FinishDrawTarget's mip DrawRects, DxCam::Select's RndCam::Select) or
// while the game is held for a render check (HoldIfRequested)
std::mutex g_state_mutex;

std::atomic<bool> g_enabled{false};
// a texture pass is open and its draws are recorded (State::open.record), so
// the draw hooks record even while capture is off; written under
// g_state_mutex, read before it by their early-outs
std::atomic<bool> g_pass_recording{false};
// RndShader::Cache's last option word and ShaderType, per thread: a draw's
// shader is the last one its own thread cached (the main thread may load
// while the splash thread draws)
thread_local uint64_t g_shader_options = 0;
thread_local int32_t g_shader_type = -1;
// The material RndShader::SelectConfig last selected on this thread (r3,
// null for none): a material pass's, which DxMesh::DrawShowing selects
// before each DrawFaces. Stored on every call, as it's one store.
thread_local uint32_t g_selected_mat = 0;
// The DxMesh::DrawShowing running on this thread (mesh 0 outside one) and
// the passes it has drawn: each DxMesh::DrawFaces inside it is a pass, a
// draw of its own (CaptureMesh)
struct MeshDrawing {
    uint32_t mesh = 0;
    uint32_t passes = 0;
};
thread_local MeshDrawing g_mesh_drawing;
// The DxMultiMesh::DrawShowing running on this thread (0 outside one), its
// passes selected so far and the last one's material: DrawBatchedNewGfx draws
// a pass's instances between one SelectConfig and the next, so a pass is
// recorded at the next one's SelectConfig, and the last at the end
struct MultiMeshDrawing {
    uint32_t multimesh = 0;
    uint32_t passes = 0;
    uint32_t mat = 0;
};
thread_local MultiMeshDrawing g_multimesh_drawing;
// native_view_rt_fallback, kept by its change callback
std::atomic<bool> g_rt_fallback_guest{true};
// the anisotropy textures are sampled with (guest_formats.h's
// DecodeSampler): native_anisotropic, or the emulated GPU's
// anisotropic_override (NativeAnisotropy), kept by their change callbacks
std::atomic<int32_t> g_aniso_override{-1};
// native_view_record_targets: texture passes are recorded while capture is
// off too (off by default, when the game pays only the hooks' early-outs)
std::atomic<bool> g_record_targets{false};

// whether the texture-pass hooks do anything: while capturing, and all the
// time with native_view_record_targets
bool Active() {
    return g_enabled.load(std::memory_order_relaxed) ||
           g_record_targets.load(std::memory_order_relaxed);
}
// whether the draw hooks do: while capturing, and inside a recorded pass
bool Recording() {
    return g_enabled.load(std::memory_order_relaxed) ||
           g_pass_recording.load(std::memory_order_relaxed);
}
// PassRecordingStats, written on the game's render thread
std::atomic<uint64_t> g_rec_passes{0}, g_rec_recorded{0}, g_rec_draws{0}, g_rec_ns{0};

// times the always-on recording: only while capture is off and a pass is
// recorded, so the clock is read for little more than its own draws
class RecordTimer {
 public:
    RecordTimer()
        : on_(g_pass_recording.load(std::memory_order_relaxed) &&
              !g_enabled.load(std::memory_order_relaxed)) {
        if (on_) start_ = std::chrono::steady_clock::now();
    }
    ~RecordTimer() {
        if (!on_) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_).count();
        g_rec_ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
    }

 private:
    bool on_;
    std::chrono::steady_clock::time_point start_;
};
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
    bool fell_back = false;  // none was one it waited for: frame is the last it saw
    bool released = true;
};
HeldRequest g_held;
// frames that don't say what they drew (frame_compose.h's ProcKnown): looked
// at before choosing, to learn how many draws a full one has
constexpr int kFramesToLearn = 2;
// after this many, any frame will do
constexpr int kMaxFramesToWait = 30;
// the game is never held longer than this, even if the requester goes away
constexpr std::chrono::seconds kMaxHold{3};
std::mutex g_latest_mutex;
std::shared_ptr<const FrameCapture> g_latest;
// when g_latest was published, for the native renderer's latency
std::chrono::steady_clock::time_point g_latest_published;
// captures published and WakeCaptureWaiters calls so far, under
// g_latest_mutex: the native renderer's worker waits on g_latest_cv for it to
// move rather than looking every few milliseconds
uint64_t g_capture_epoch = 0;
std::condition_variable g_latest_cv;
// native_view_record_targets as set, and whether renderer has been native at
// any time this session: texture passes are recorded if either is
// (g_record_targets)
std::atomic<bool> g_record_targets_set{false}, g_renderer_was_native{false};
// The game's frames: when the newest few thousand DxRnd::Presents ended, and
// how many there have been, for the harness's present_stats
std::mutex g_present_times_mutex;
PresentTimes g_present_times;

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

        const uint64_t verts = HashBytes(vsrc, std::min<uint32_t>(vb_bytes, 256));
        const uint64_t indices = HashBytes(isrc, std::min<uint32_t>(num_indices * 2, 64));
        const uint64_t key =
            Key({vb_phys, vb_bytes, ib_addr, ib_bytes, num_faces, uint32_t(verts),
                 uint32_t(verts >> 32), uint32_t(indices), uint32_t(indices >> 32)});
        auto it = S().geoms.find(geom);
        if (it != S().geoms.end() && it->second.key == key) {
            fc.geom_cached++;
            Lap(S(), CaptureProfile::kStepGeomHit);
            return it->second.geom;
        }
        // the buffers' bytes copied, decoded off this thread (deferred_decode.h):
        // a song's or a shot's first frame decoding its meshes here cost the
        // game's thread up to 16 MB of Vertex, several milliseconds
        auto out = std::make_shared<Geometry>();
        S().profile.allocs++;
        out->tangents = true;
        auto d = std::make_shared<DeferredGeometry>();
        d->num_verts = num_verts;
        d->num_indices = num_indices;
        d->vb.assign(vsrc, vsrc + size_t(num_verts) * kPackedVert_Size);
        d->ib.assign(isrc, isrc + size_t(num_indices) * 2);
        d->faces = guest_format::HasKeptFace(num_verts, isrc, num_indices);
        S().profile.geom_copy_bytes += d->vb.size() + d->ib.size();
        out->deferred = std::move(d);
        S().geoms[geom] = GeomEntry{key, out};
        Lap(S(), CaptureProfile::kStepGeomMiss);
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
    out->tangents = true;
    out->indices.reserve(num_faces_cpu * 3);
    for (uint32_t i = 0; i < num_faces_cpu; i++) {
        const uint16_t a = g.U16(faces + i * 6), b = g.U16(faces + i * 6 + 2),
                       c = g.U16(faces + i * 6 + 4);
        if (a >= num_verts || b >= num_verts || c >= num_verts) continue;
        out->indices.insert(out->indices.end(), {a, b, c});
    }
    fc.mutable_meshes++;
    S().profile.allocs++;
    Lap(S(), CaptureProfile::kStepGeomMutable);
    return out;
}

// The fetch constant's fields that say what its texture's texels are, as
// guest_formats.h's ReadFetchLayout and DecodeTextureLevels read them: tiling
// and pitch (dword 0), format, endianness and base (1), size (2), swizzle
// (3), the levels kept (4), dimension, packed mips and the mip address (5).
// Not the sampler's (clamp, filters, anisotropy, LOD bias, border), which
// draws sampling the same texture set their own ways: keyed on those too, a
// texture two draws sample differently was decoded again at each (7 ms a
// frame in the main hub, its 512x512 maps over and over).
constexpr uint32_t kFetchTexelBits[6] = {0xFFC00000u, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                         0x00001FFEu, 0x000003FCu, 0xFFFFFE00u};

// deferred_decode.h decodes what CopyForLater keeps with DecodeTextureLevels'
// own limit
static_assert(kMaxTextureSize <= 4096, "deferred textures decode at up to 4096 texels a side");

// `n` bytes of guest memory from `src` (`address`, a fetch constant's base or
// mip address) into `out`, as far as guest memory goes (its 512 MB of
// physical memory, or the 4 GB of addresses whose views of it RB3's are),
// zeros after: what DecodeTextureLevels would read there in place
void CopyGuestBytes(const uint8_t* src, uint32_t address, uint32_t n, std::vector<uint8_t>& out) {
    const uint64_t left = address >= 0xA0000000u ? (uint64_t(1) << 32) - address
                                                 : 0x20000000u - (address & 0x1FFFFFFFu);
    const size_t copied = size_t(std::min<uint64_t>(n, left));
    out.assign(src, src + copied);
    out.resize(n, 0);
}

// A texture with its base level's and its mip chain's bytes copied as guest
// memory holds them now (guest_formats.h's BaseLevelBytes, MipChainBytes),
// for DecodeDeferred to decode later (Texture::deferred), or null where
// DecodeTexture wouldn't decode it. Its mips are copied where DecodeTexture
// would read them (a mip address and levels under it, there in memory).
std::shared_ptr<Texture> CopyForLater(const Guest& g, const uint8_t* src, const uint32_t f[6]) {
    const guest_format::FetchLayout l = guest_format::ReadFetchLayout(f);
    guest_format::FormatInfo info{};
    const uint32_t bytes = guest_format::BaseLevelBytes(f);
    if (!src || !bytes || l.dimension != 1 || !guest_format::GetFormatInfo(l.format, info) ||
        l.width > kMaxTextureSize || l.height > kMaxTextureSize)
        return nullptr;
    auto tex = std::make_shared<Texture>();
    tex->width = l.width;
    tex->height = l.height;
    tex->format = l.format;
    auto d = std::make_shared<DeferredPixels>();
    CopyGuestBytes(src, l.base_address, bytes, d->bytes);
    if (l.mip_address && l.mip_max)
        if (const uint8_t* mips = GpuHost(g, l.mip_address))
            CopyGuestBytes(mips, l.mip_address, guest_format::MipChainBytes(f), d->mips);
    std::copy(f, f + 6, d->fetch);
    tex->deferred = std::move(d);
    return tex;
}

// the texture fetch constant `f` describes, decoded again only when its
// texels' fields (kFetchTexelBits) or its first bytes changed, or with `whole`
// any of its base level's: a movie's plane, which the CPU writes in place
// (Movie.cpp's BeginFrame, into one of four buffers in turn), and whose first
// rows are often the same black from one frame to the next. Not every
// texture: hashing each of 256 KB or less whole cost the capture 5-11 ms a
// frame in the music library (30 MB: each draw's), a movie's planes cost it
// 0.07 ms there (384 KB; 1.4 MB at 1280x720). With `defer`, a new or changed
// texture's bytes are only copied (CopyForLater), and decoded off the game's
// thread (deferred_decode.h): decoding a video venue's 1280x720 planes here
// cost the game's thread 5-6 ms a frame, and a song's or a shot's first
// frame's textures 22-47 MB of RGBA, 30-60 ms. Empty rgba if its format
// isn't decoded, but for a deferred one, whose rgba the game's thread
// mustn't read (HasPixels).
std::shared_ptr<const Texture> DecodeCached(const Guest& g, const uint32_t f[6], uint32_t where,
                                            std::unordered_map<uint32_t, TexEntry>& cache,
                                            FrameCapture& fc, bool whole = false,
                                            bool defer = false) {
    const uint32_t base_address = f[1] & 0xfffff000u;
    const uint8_t* src = base_address ? GpuHost(g, base_address) : nullptr;
    uint64_t texels = 0;
    if (src) {
        const uint32_t bytes = guest_format::BaseLevelBytes(f);
        texels = HashBytes(src, whole && bytes ? bytes : 64);
    }
    uint32_t k[6];
    for (int i = 0; i < 6; i++) k[i] = f[i] & kFetchTexelBits[i];
    const uint64_t key =
        Key({k[0], k[1], k[2], k[3], k[4], k[5], uint32_t(texels), uint32_t(texels >> 32)});
    auto it = cache.find(where);
    State& s = S();
    if (it != cache.end() && it->second.key == key) {
        fc.tex_cached++;
        Lap(s, CaptureProfile::kStepTexLookup);
        return it->second.tex;
    }
    Lap(s, CaptureProfile::kStepTexLookup);
    std::shared_ptr<const Texture> tex = defer ? CopyForLater(g, src, f) : nullptr;
    if (tex) {
        s.profile.tex_copy_bytes += tex->deferred->bytes.size() + tex->deferred->mips.size();
    } else {
        tex = DecodeTexture(g, f);
        uint64_t bytes = tex->rgba.size() * 4;
        for (const auto& level : tex->mips) bytes += level.size() * 4;
        s.profile.tex_decode_bytes += bytes;
    }
    cache[where] = TexEntry{key, tex};
    s.profile.allocs++;
    Lap(s, CaptureProfile::kStepTexDecode);
    return tex;
}

// whether a texture has pixels to draw, or will have (decoded later): what
// the game's thread asks, which never reads a deferred one's rgba, as
// another thread may be decoding it
bool HasPixels(const Texture& t) { return t.deferred || !t.rgba.empty(); }

// whether a mesh's geometry has faces to draw, decoded or not (as
// HasPixels): what the game's thread asks of geometry it has captured
bool HasFaces(const Geometry& g) { return g.deferred ? g.deferred->faces : !g.indices.empty(); }

// RndTex::Type's kMovie bit: a movie's plane, which the CPU writes
constexpr uint32_t kTexTypeMovie = 4;

// a loaded texture's pixels, decoded (or null, counted, in a format that isn't)
std::shared_ptr<const Texture> GuestPixels(const Guest& g, uint32_t tex_obj, FrameCapture& fc) {
    const uint32_t d3d = g.U32(tex_obj + kDxTex_Texture);
    if (!d3d) return nullptr;
    uint32_t f[6];
    for (int i = 0; i < 6; i++) f[i] = g.U32(d3d + kD3DBaseTexture_Fetch + i * 4);
    const uint32_t type = g.U32(tex_obj + kTex_Type);
    const bool movie = (type & kTexTypeMovie) != 0;
    // a render target's pixels are copied on this thread (CaptureTexture), so
    // decoded here; any other's later, off it
    std::shared_ptr<const Texture> tex =
        DecodeCached(g, f, d3d, S().texs, fc, movie, !IsRenderedType(type));
    if (!HasPixels(*tex)) {
        fc.untextured_format++;
        return nullptr;
    }
    return tex;
}

// A draw's diffuse texture. A loaded one is decoded from guest memory; one RB3
// draws at runtime is its identity and version (Texture::tex_obj), with guest
// memory's pixels only if native_view_rt_fallback is guest and renderer is
// emulated (RtFallbackGuest), and a sample the capture has to have the pass
// of. Null when there's nothing to draw it with.
std::shared_ptr<const Texture> CaptureTexture(const Guest& g, State& s, Sink& sink,
                                              uint32_t tex_obj) {
    FrameCapture& fc = sink.fc;
    const uint32_t type = g.U32(tex_obj + kTex_Type);
    if (!IsRenderedType(type)) {
        std::shared_ptr<const Texture> tex = GuestPixels(g, tex_obj, fc);
        if (tex) fc.textured++;
        return tex;
    }
    uint32_t version = 0;
    if (IsPassTargetType(type)) {
        auto it = s.rts.find(tex_obj);
        if (it != s.rts.end()) version = it->second.version;
        sink.samples.push_back(RtKey(tex_obj, version));
    } else {
        fc.rt_snapshots++;
    }
    std::shared_ptr<const Texture> pixels;
    if (RtFallbackGuest()) pixels = GuestPixels(g, tex_obj, fc);
    if (pixels) fc.textured++;

    // the same version with the same pixels is the same Texture, so a frame's
    // draws of it share one (a crowd type's billboards, a character's pieces)
    RtState& rt = s.rts[tex_obj];
    if (rt.sampled && rt.sampled->version == version && rt.sampled->tex_type == type &&
        rt.sampled_pixels == pixels) {
        fc.tex_cached++;
        Lap(s, CaptureProfile::kStepTexRt);
        return rt.sampled;
    }
    auto tex = std::make_shared<Texture>();
    s.profile.allocs++;
    tex->tex_obj = tex_obj;
    tex->tex_type = type;
    tex->version = version;
    if (pixels) {
        // (decoded here, as GuestPixels decodes a render target's; one found
        // deferred in the cache, from another fetch of the same texture, is
        // decoded now, the worker waiting for it if it's there too)
        if (pixels->deferred) DecodeDeferred(*pixels);
        tex->width = pixels->width;
        tex->height = pixels->height;
        tex->format = pixels->format;
        tex->rgba = pixels->rgba;
    } else {
        tex->width = g.U32(tex_obj + kTex_Width);
        tex->height = g.U32(tex_obj + kTex_Height);
    }
    rt.sampled = tex;
    rt.sampled_pixels = pixels;
    Lap(s, CaptureProfile::kStepTexRt);
    return tex;
}

// the view-projection the device's VS has (kVsViewProj), its columns read
// back into rows
Mat4 DeviceViewProj(const Guest& g) {
    Mat4 m = Identity();
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return m;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m.m[r][c] = g.F32(dev + kDev_VertexShaderF + (kVsViewProj + c) * 16 + r * 4);
    return m;
}

// The camera's view-projection, read the first time a draw needs it after a
// camera select: DxCam::Select writes it after RndCam::Select. Identity
// without a camera. A texture pass no camera selected (NgLight's shadow, which
// uploads its own) draws with the device's.
const Mat4& ViewProj(const Guest& g, State& s) {
    if (s.open.tex && s.open.rec && !s.open.rec->pass.cam) {
        s.vp = DeviceViewProj(g);
        s.vp_valid = false;  // a camera's again after
        return s.vp;
    }
    if (!s.vp_valid) {
        s.vp = s.cam ? ReadMatrix4(g, s.cam + kCam_ViewProj) : Identity();
        s.vp_valid = true;
    }
    return s.vp;
}

// Where a draw goes, or false (and counted) if it isn't recorded: into the
// texture pass the game has open while one is (whatever the camera), else into
// the frame's back buffer while capturing, if the current camera draws there;
// and only in a colour pass, the normal one or a reflection's, or, for a
// particle system (`particles`), the soft-particle buffer's pass into its
// surface (draw mode 6: IsSoftParticle), or a shadow's: draw mode 1 into a
// shadow map's pass (RndShadowMap's depth), 3 into any (NgLight's casters).
// Other draws there (a mesh child of RndSoftParticles; none seen) are left
// out. The sink says which mode it was. A DrawRect quad (`rect`) goes to the
// back buffer whatever the camera, outside a texture pass: it draws in the
// target's pixels with no camera's transform, and RB3 draws its ScreenMasks
// and post copy after a shadow map's camera was the last selected.
bool Target(const Guest& g, State& s, std::optional<Sink>& sink, bool particles = false,
            bool rect = false) {
    if (s.open.tex) {
        if (!s.open.record) return false;
        PassRecord& rec = *s.open.rec;
        sink.emplace(Sink{rec.content, s.open.shades, rec.samples, s.open.tex});
    } else {
        if (!g_enabled.load(std::memory_order_relaxed)) return false;
        FrameCapture& fc = *s.building;
        if (!rect && (!s.cam || !s.cam_backbuffer)) {
            fc.skipped_target++;
            return false;
        }
        sink.emplace(Sink{fc, s.shades, s.samples, 0});
    }
    FrameCapture& fc = sink->fc;
    const uint32_t holder = g.U32(kDrawModeHolder);
    const uint32_t mode = holder ? g.U32(holder + kDrawMode) : kDrawModeNormal;
    const bool soft = mode == kDrawModeSoftParticles && particles && s.soft_surface &&
                      s.open.tex == s.soft_surface;
    const bool shadow_depth = mode == kDrawModeShadowDepth && s.open.tex && s.open.rec &&
                              s.open.rec->pass.tex_type == kTexTypeShadowMap;
    const bool casters = mode == kDrawModeShadowCasters && s.open.tex;
    if (mode != kDrawModeNormal && mode != kDrawModeReflection && !soft && !shadow_depth &&
        !casters) {
        if (mode == kDrawModeVelocity) {
            fc.skipped_velocity++;
        } else if (mode == kDrawModeShadowDepth || mode == kDrawModeShadowCasters) {
            fc.skipped_shadow++;
        } else {
            fc.skipped_draw_mode++;
        }
        return false;
    }
    if (!sink->target && s.cam_backbuffer && !s.cam_counted) {
        fc.cams++;
        s.cam_counted = true;
    }
    sink->draw_mode = uint8_t(mode);
    return true;
}

// a draw into its sink; the frame's back-buffer draws start a pass of their
// own after a texture pass
void PushDraw(State& s, Sink& sink, DrawItem&& item) {
    FrameCapture& fc = sink.fc;
    if (!sink.target) {
        if (fc.passes.empty() || fc.passes.back().tex_obj) {
            Pass p;
            p.first_draw = uint32_t(fc.draws.size());
            p.from_frame = s.game_frame;
            fc.passes.push_back(p);
        }
        fc.passes.back().draw_count++;
    } else if (!g_enabled.load(std::memory_order_relaxed)) {
        g_rec_draws.fetch_add(1, std::memory_order_relaxed);
    }
    item.draw_mode = sink.draw_mode;
    fc.draws.push_back(std::move(item));
    s.profile.draws++;
    Lap(s, CaptureProfile::kStepPush);
}

// an address in a texture's header as the device's fetch constants have it
// once the texture is set: physical, the CPU's 0xE0000000 view 0x1000 on (see
// GpuHost)
uint32_t GpuPhysical(uint32_t address) {
    if (address < 0xA0000000u) return address;
    return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0);
}

// the physical base address of a DxTex's texture, 0 without one
uint32_t TexBase(const Guest& g, uint32_t tex_obj) {
    const uint32_t d3d = tex_obj ? g.U32(tex_obj + kDxTex_Texture) : 0;
    const uint32_t base = d3d ? g.U32(d3d + kD3DBaseTexture_Fetch + 4) & 0xfffff000u : 0;
    return base ? GpuPhysical(base) : 0;
}

// whether a shader with these options samples the map (ShaderOptions.cpp's
// macros; the samplers per rb3-xenon Mat_NG.cpp and Env_NG.cpp)
bool MapSampled(const ShadeInputs& in, int map) {
    using namespace shader_opt;
    const bool proj = in.OptionBits(kNumProj, 2) != 0;
    switch (map) {
        case kMapNormal: return in.Option(kNormalMap);
        case kMapSpecular: return in.Option(kSpecularMap);
        case kMapGlow: return in.Option(kGlowMap);
        case kMapEnviron: return in.Option(kEnvironMap);
        case kMapProjected: return proj || in.Option(kShadowBuffer);
        case kMapGobo: return proj && !in.Option(kProjLightMultiply);
        case kMapDetailNormal: return in.Option(kNormDetail);
        case kMapRim: return in.Option(kRimLightMap);
        default: return false;
    }
}

// a 2D map the device has bound, decoded off this thread (and counted), or
// null for a cube or a format not decoded; `whole` a movie's plane
// (DecodeCached)
std::shared_ptr<const Texture> CaptureMap(const Guest& g, const uint32_t f[6],
                                          FrameCapture& fc, bool whole = false) {
    const uint32_t dimension = (f[5] >> 9) & 3;
    if (dimension == 3) {
        fc.maps_cube++;
        return nullptr;
    }
    std::shared_ptr<const Texture> tex =
        DecodeCached(g, f, f[1] & 0xfffff000u, S().map_texs, fc, whole, true);
    if (!tex || !HasPixels(*tex)) {
        fc.maps_other_format++;
        return nullptr;
    }
    fc.maps_decoded++;
    return tex;
}

// a map kept as a render target's identity (Texture::tex_obj), or null
const Texture* RtMap(const std::shared_ptr<const Texture>& t) {
    return t && t->tex_obj ? t.get() : nullptr;
}

// `shade`'s index in fc.shades, which gets it if no equal one is there yet:
// the same inputs, and the same render targets' versions where it has some
// as maps already (the same inputs can read another version of one: each
// character's shadow map); fill_maps decodes a new one's other maps. `draw`:
// a draw's (CaptureShade), whose steps are timed as the shade's, not a pass's
// copied in (AppendPass)
template <typename FillMaps>
int32_t InternShade(FrameCapture& fc, ShadeIndex& index, ShadeState&& shade, FillMaps fill_maps,
                    bool draw = false) {
    const ShadeInputs& in = shade;
    const uint64_t hash = HashBytes(&in, sizeof(in));
    auto [first, last] = index.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        const ShadeState& other = fc.shades[it->second];
        if (std::memcmp(static_cast<const ShadeInputs*>(&other), &in, sizeof(in)) != 0) continue;
        bool same_rts = true;
        for (int m = 0; m < kNumShadeMaps; m++)
            same_rts &= RtMap(other.maps[m]) == RtMap(shade.maps[m]);
        if (same_rts) {
            if (draw) Lap(S(), CaptureProfile::kStepShadeIntern);
            return it->second;
        }
    }
    if (draw) Lap(S(), CaptureProfile::kStepShadeIntern);
    fill_maps(shade);
    const int32_t i = int32_t(fc.shades.size());
    fc.shades.push_back(std::move(shade));
    index.emplace(hash, i);
    if (draw) {
        S().profile.new_shades++;
        Lap(S(), CaptureProfile::kStepShadeStore);
    }
    return i;
}

// What the draw just made had its shader read, from the device's constant
// shadow: the hooks run after the draw, and RndShader::SelectConfig sets the
// constants per material pass, so a mesh's are read after each pass's
// DrawFaces (and a multimesh's before the next pass's SelectConfig). The spotlight
// drawer's registers are kept for its cones (ShaderType 2) and, `blur`, a
// DrawRect blur's taps, c89 (the camera's depth range) for a soft particle,
// and zeroed for the rest. `mat` 0 for a draw without a material (a cone);
// `default_mat` for a mesh's pass without one, drawn with TheRnd's default
// material (`mat`), which ShadeInputs::mat then has as kDefaultMaterial
// (NoMaterial).
// The index of an equal state already in the sink, or of a new one; -1
// without a device.
int32_t CaptureShade(const Guest& g, State& s, Sink& sink, uint32_t mat, bool blur = false,
                     bool default_mat = false) {
    FrameCapture& fc = sink.fc;
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return -1;
    ShadeState shade;
    ShadeInputs& in = shade;
    std::memset(&in, 0, sizeof(in));
    in.options = g_shader_options;
    in.shader_type = g_shader_type;
    in.env = g.U32(kEnvironCurrent);
    // no camera yet: a DrawRect drawn before the first RndCam::Select after
    // capture came on (nothing keeps s.cam while it's off), at 0 in guest
    // memory that isn't there
    if (s.cam)
        for (int i = 0; i < 3; i++) in.eye[i] = g.F32(s.cam + kTrans_WorldXfm + 0x30 + i * 4);
    for (int r = 0; r < kNumShadeRegs; r++) {
        for (int c = 0; c < 4; c++) {
            in.vs[r][c] = g.F32(dev + kDev_VertexShaderF + kShadeRegs[r] * 16 + c * 4);
            in.ps[r][c] = g.F32(dev + kDev_PixelShaderF + kShadeRegs[r] * 16 + c * 4);
        }
    }
    if (in.shader_type != kDepthVolumeShader && !blur) {
        const bool soft =
            in.shader_type == kParticleShader && in.Option(shader_opt::kSoftParticles);
        for (int r = kFirstSpotShadeReg; r < kNumShadeRegs; r++) {
            if (soft && kShadeRegs[r] == 89) continue;
            std::memset(in.vs[r], 0, sizeof(in.vs[r]));
            std::memset(in.ps[r], 0, sizeof(in.ps[r]));
        }
    }
    in.mat = default_mat ? kDefaultMaterial : mat;
    if (mat) {
        in.next_pass = g.U32(mat + kMat_NextPass);
        in.use_environ = g.U8(mat + kMat_UseEnviron);
        in.intensify = g.U8(mat + kMat_Intensify);
        in.per_pixel_lit = g.U8(mat + kMat_PerPixelLit);
        in.alpha_write = g.U8(mat + kMat_AlphaWrite);
        in.shader_variation = int32_t(g.U32(mat + kMat_ShaderVariation));
        in.mat_diffuse_base = TexBase(g, g.U32(mat + kMat_DiffuseTex));
    }
    auto fetch = [&](uint32_t sampler, uint32_t out[6]) {
        for (int i = 0; i < 6; i++) out[i] = g.U32(dev + kDev_TextureFetch + sampler * 24 + i * 4);
    };
    // the movie's shader samples its three planes whatever the option word
    // (0): Y s0, cR s2, cB s3 (IsMovie)
    const bool movie = in.shader_type == kMovieShader;
    // REFRACT_WORLD's samples s1 too, its material's refract normal map
    // (NgMat::SetupShader binds it with c119 for the shader, which moves
    // where it reads the picture behind by it), whatever NORMAL_MAP says
    const bool refract = in.Option(shader_opt::kRefractWorld);
    if (in.Option(shader_opt::kDiffuseMap) || movie) fetch(0, in.fetch_diffuse);
    for (int m = 0; m < kNumShadeMaps; m++) {
        if (kMat_Map[m] && mat) {
            in.mat_maps[m] = g.U32(mat + kMat_Map[m]);
            // RndCubeTex isn't a DxTex
            if (m != kMapEnviron) in.mat_map_base[m] = TexBase(g, in.mat_maps[m]);
        }
        if (MapSampled(in, m) || (movie && (m == kMapSpecular || m == kMapGlow)) ||
            (refract && m == kMapNormal))
            fetch(kShadeMapSampler[m], in.fetch[m]);
    }
    Lap(s, CaptureProfile::kStepShadeRead);

    // s5, s1 or s14 bound to a texture a pass draws (the shadow map,
    // NgLight's shadow, a head's normal map): its identity and the version it
    // has now, as a diffuse render target's, and a sample the capture has to
    // have the pass of
    for (int m : {kMapProjected, kMapNormal, kMapDetailNormal}) {
        const uint32_t base = in.fetch[m][1] & 0xfffff000u;
        if (!base) continue;
        for (const auto& [tex, rt] : s.rts) {
            if (rt.base != base) continue;
            Lap(s, CaptureProfile::kStepShadeRtsScan);
            shade.maps[m] = CaptureTexture(g, s, sink, tex);
            break;
        }
    }
    Lap(s, CaptureProfile::kStepShadeRtsScan);
    return InternShade(
        fc, sink.shades, std::move(shade),
        [&](ShadeState& st) {
            // the samplers the textures are read with, from the same fetch
            // constants (none bound: the default)
            const int32_t aniso = g_aniso_override.load(std::memory_order_relaxed);
            st.diffuse_sampler = guest_format::DecodeSampler(st.fetch_diffuse, aniso);
            for (int m = 0; m < kNumShadeMaps; m++)
                st.samplers[m] = guest_format::DecodeSampler(st.fetch[m], aniso);
            Lap(s, CaptureProfile::kStepShadeFill);
            for (int m = 0; m < kNumShadeMaps; m++)
                if (st.fetch[m][1] && !st.maps[m])
                    st.maps[m] = CaptureMap(g, st.fetch[m], fc, movie);
            Lap(s, CaptureProfile::kStepShadeFill);
        },
        true);
}

// a draw of `geometry` with material `mat`, for the current camera;
// sample_texture false leaves its diffuse texture out (a mip downsample's,
// which is the texture it's drawing); blur: a DrawRect blur, whose taps its
// shade state keeps; default_mat: `mat` is TheRnd's default, for a mesh's
// pass without a material (CaptureShade)
DrawItem MakeItem(const Guest& g, State& s, Sink& sink, uint32_t mat, uint32_t owner,
                  std::shared_ptr<const Geometry> geometry, bool sample_texture = true,
                  bool blur = false, bool default_mat = false) {
    DrawItem item;
    item.geom = std::move(geometry);
    item.world = Identity();
    item.view_proj = ViewProj(g, s);
    for (int i = 0; i < 4; i++) item.color[i] = g.F32(mat + kMat_Color + i * 4);
    item.blend = int(g.U32(mat + kMat_Blend));
    item.z_mode = int(g.U32(mat + kMat_ZMode));
    item.prelit = g.U8(mat + kMat_Prelit) != 0;
    item.alpha_cut = g.U8(mat + kMat_AlphaCut) != 0;
    item.alpha_threshold = int(g.U32(mat + kMat_AlphaThreshold));
    item.cam = s.cam;
    item.mesh = owner;
    item.target = sink.target;
    // (a shadow map's depth samples nothing: kShadowmapShader has SKINNED alone)
    const uint32_t tex = g.U32(mat + kMat_DiffuseTex);
    Lap(s, CaptureProfile::kStepItem);
    if (tex && sample_texture && sink.draw_mode != kDrawModeShadowDepth)
        item.tex = CaptureTexture(g, s, sink, tex);
    item.shade = CaptureShade(g, s, sink, mat, blur, default_mat);
    return item;
}

// the cull mode the draw just made had (DrawItem::cull): RndShader::SelectConfig
// sets it per material pass, so read as the constants are (CaptureShade)
uint8_t CaptureCull(const Guest& g) {
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    return dev ? uint8_t(g.U32(dev + kDev_ModeCntl) & (kCullFront | kCullBack | kCullFrontIsCw))
               : 0;
}

// the mesh's geometry for a pass with material `mat`, or false (and counted)
// if it draws nothing: fur, or no geometry. A pass without a material
// (RndShader::SelectConfig(null), a mesh without one) draws with TheRnd's
// default material (every RndShader's Select takes it for null): `mat`
// becomes that one, and `default_mat` says so (counted, skipped_no_mat).
bool MeshParts(const Guest& g, FrameCapture& fc, uint32_t mesh, uint32_t& mat,
               bool& default_mat, std::shared_ptr<const Geometry>& geometry) {
    uint32_t geom = g.U32(mesh + kMesh_GeomOwner);
    if (!geom) geom = mesh;
    default_mat = !mat;
    if (!mat) {
        fc.skipped_no_mat++;
        const uint32_t rnd = g.U32(kDrawModeHolder);
        mat = rnd ? g.U32(rnd + kRnd_DefaultMat) : 0;
    }
    if (!mat || g.U32(mat + kMat_Fur)) {
        fc.skipped_no_geom++;
        return false;
    }
    geometry = CaptureGeometry(g, geom, fc);
    if (!geometry || !HasFaces(*geometry)) {
        fc.skipped_no_geom++;
        return false;
    }
    return true;
}

// A spotlight's cone (scene_capture.h's IsSpotCone), drawn while the depth
// volume's pass is open: NgSpotlightDrawer::RenderConeDefs draws the beam's
// proxy mesh through DxMesh::DrawShowing with no material, ShaderType 2 and
// the device set up by hand (blend ONE ONE, no Z test or write, cull as
// RndShader's override left it). Its cross-section texture is s11, when PS
// c86.x says the shader samples it.
void CaptureSpotCone(const Guest& g, State& s, Sink& sink, uint32_t mesh) {
    FrameCapture& fc = sink.fc;
    uint32_t geom = g.U32(mesh + kMesh_GeomOwner);
    if (!geom) geom = mesh;
    std::shared_ptr<const Geometry> geometry = CaptureGeometry(g, geom, fc);
    if (!geometry || !HasFaces(*geometry)) {
        fc.skipped_no_geom++;
        return;
    }
    DrawItem item;
    item.geom = std::move(geometry);
    item.world = ReadXfm(g, mesh + kMesh_WorldXfm);
    item.view_proj = ViewProj(g, s);
    for (float& c : item.color) c = 1.0f;
    item.blend = 2;   // kBlendAdd
    item.z_mode = 0;  // kZModeDisable
    item.prelit = true;
    item.alpha_cut = false;
    item.alpha_threshold = 0;
    item.cam = s.cam;
    item.mesh = mesh;
    item.target = sink.target;
    item.cull = CaptureCull(g);
    if (const uint32_t dev = g.U32(kD3DDeviceHolder);
        dev && g.F32(dev + kDev_PixelShaderF + kSpotXsecWeightReg * 16) > 0) {
        uint32_t f[6];
        for (int i = 0; i < 6; i++)
            f[i] = g.U32(dev + kDev_TextureFetch + kSpotXsecSampler * 24 + i * 4);
        if (f[1]) item.tex = CaptureMap(g, f, fc);
    }
    item.shade = CaptureShade(g, s, sink, 0);
    PushDraw(s, sink, std::move(item));
}

std::string ReadName(const Guest& g, uint32_t p);

// With BAND3_NATIVE_VIEW_LOG_DRAWS set, the first 32 of each captured
// frame's draws of a kind (`logged`, State's) are logged: the passes drawn
// without a material, and DxMesh::DrawFaces outside a DrawShowing (which are
// logged without it too, the session's first 8)
bool DiagLog(uint32_t& logged) {
    static const bool on = std::getenv("BAND3_NATIVE_VIEW_LOG_DRAWS") != nullptr;
    if (!on || !g_enabled.load(std::memory_order_relaxed) || logged >= 32) return false;
    logged++;
    return true;
}

// The name of an object whose Hmx::Object is a virtual base (a RndMesh's,
// under RndDrawable's and RndTransformable's RndHighlightable), for the logs:
// the vbtable its vbptr (+4) points at has the virtual bases' offsets from
// the vbptr, and Hmx::Object's mName is at its +0x18; the first entry that
// gives a printable name
std::string VirtualBaseName(const Guest& g, uint32_t obj) {
    const uint32_t table = obj ? g.U32(obj + 4) : 0;
    if (table < 0x82000000u || table >= 0x84000000u) return {};
    for (int k = 1; k <= 3; k++) {
        const int32_t off = int32_t(g.U32(table + 4 * k));
        if (off <= 0 || off > 0x1000) continue;
        const std::string name = ReadName(g, g.U32(obj + 4 + uint32_t(off) + kObj_Name));
        if (!name.empty() && std::all_of(name.begin(), name.end(),
                                         [](char c) { return c >= 0x20 && c < 0x7f; }))
            return name;
    }
    return {};
}

// a pass without a material (MeshParts' default_mat) logged: the mesh, its
// geometry, where it is and where its vertices land on the screen
void LogNoMaterial(const Guest& g, const State& s, const Sink& sink, const DrawItem& it,
                   const char* kind) {
    const Geometry& geom = *it.geom;
    // (its verts decoded here, a diagnostic's cost)
    if (geom.deferred) DecodeDeferred(geom);
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    uint32_t front = 0, colors = 0;
    for (const Vertex& v : geom.verts) {
        const Mat4& w = it.bones.empty() ? it.world
                                         : it.bones[v.bone[0] < it.bones.size() ? v.bone[0] : 0];
        float p[3], c[4];
        for (int i = 0; i < 3; i++)
            p[i] = v.pos[0] * w.m[0][i] + v.pos[1] * w.m[1][i] + v.pos[2] * w.m[2][i] + w.m[3][i];
        for (int i = 0; i < 4; i++)
            c[i] = p[0] * it.view_proj.m[0][i] + p[1] * it.view_proj.m[1][i] +
                   p[2] * it.view_proj.m[2][i] + it.view_proj.m[3][i];
        colors |= v.color ^ geom.verts[0].color;
        if (c[3] <= 1e-3f) continue;
        front++;
        for (int i = 0; i < 2; i++) {
            lo[i] = std::min(lo[i], c[i] / c[3]);
            hi[i] = std::max(hi[i], c[i] / c[3]);
        }
    }
    REXLOG_INFO("native view: {} without a material: mesh {:08X} '{}', {} verts {} tris{}, "
                "colour {:08X}{}, at ({:.1f} {:.1f} {:.1f}), on screen x {:.2f}..{:.2f} y "
                "{:.2f}..{:.2f} ({} verts in front), into {:08X} ({}), camera {:08X}{}, draw "
                "mode {}, options {:016X} type {}, frame {}",
                kind, it.mesh, VirtualBaseName(g, it.mesh), geom.verts.size(),
                geom.indices.size() / 3, it.bones.empty() ? "" : " skinned",
                geom.verts.empty() ? 0u : geom.verts[0].color, colors ? " (varies)" : "",
                it.world.m[3][0], it.world.m[3][1], it.world.m[3][2], lo[0], hi[0], lo[1], hi[1],
                front, sink.target,
                s.open.tex && s.open.rec ? s.open.rec->pass.name : std::string(), s.cam,
                s.cam_backbuffer ? " (back buffer)" : "", int(sink.draw_mode), g_shader_options,
                g_shader_type, s.game_frame);
}

// a mesh without a material that drew nothing logged: `drawn` false for a
// DrawShowing that drew no pass (DxMesh::CanDraw: no buffers, not mutable),
// true for a pass whose geometry the capture couldn't have
void LogNoMaterialNotDrawn(const Guest& g, const State& s, uint32_t mesh, bool drawn) {
    uint32_t geom = g.U32(mesh + kMesh_GeomOwner);
    if (!geom) geom = mesh;
    const uint32_t faces = g.U32(geom + kMesh_Faces), faces_end = g.U32(geom + kMesh_Faces + 4);
    REXLOG_INFO("native view: a mesh without a material {}: mesh {:08X} '{}', geometry {:08X} "
                "'{}' with {} verts {} faces, mutable {}, camera {:08X}{}, frame {}",
                drawn ? "drew a pass the capture has no geometry for" : "drew no pass", mesh,
                VirtualBaseName(g, mesh), geom, VirtualBaseName(g, geom),
                g.U32(geom + kMesh_Verts + 4), (faces_end - faces) / 6,
                g.U32(geom + kMesh_Mutable), s.cam, s.cam_backbuffer ? " (back buffer)" : "",
                s.game_frame);
}

// One material pass of DxMesh::DrawShowing (rb3-xenon rnddx9/Mesh.cpp), which
// draws the mesh's faces once per pass: RndShader::SelectConfig(mat) then
// DrawFaces, for its material and each NextPass after it (0 for the first:
// pass counts them). `mat` is the pass's, null for a mesh without one;
// `drawn` false for a DrawShowing that drew no pass, recorded for what it
// counts.
// RndVelocityBuffer::DrawMesh's draw (scene_capture.h's VelocityObject):
// DxMesh::DrawShowing in draw mode 5 hands it the mesh's geometry owner,
// whose bones it counts and whose faces it draws, with the device set as
// kept here. Into the frame being captured, apart from its draws; DrawMesh
// draws none with more bones than the shader has (40).
constexpr uint32_t kMaxVelocityBones = 40;

void CaptureVelocityObject(const Guest& g, State& s, uint32_t mesh) {
    if (!g_enabled.load(std::memory_order_relaxed) || !s.building) return;
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return;
    FrameCapture& fc = *s.building;
    uint32_t owner = g.U32(mesh + kMesh_GeomOwner);
    if (!owner) owner = mesh;
    std::shared_ptr<const Geometry> geometry = CaptureGeometry(g, owner, fc);
    if (!geometry || !HasFaces(*geometry)) return;
    const uint32_t bones = g.U32(owner + kMesh_BonesBegin);
    const uint32_t bones_end = g.U32(owner + kMesh_BonesEnd);
    const uint32_t n = bones && bones_end > bones ? (bones_end - bones) / kBone_Size : 0;
    if (n > kMaxVelocityBones) return;
    VelocityObject o;
    o.geom = std::move(geometry);
    o.mesh = mesh;
    o.cull = CaptureCull(g);
    o.skinned = n > 0;
    o.bones = std::max(n, 1u);
    auto vs = [&](uint32_t reg, float* out) {
        for (int c = 0; c < 4; c++) out[c] = g.F32(dev + kDev_VertexShaderF + reg * 16 + c * 4);
    };
    for (uint32_t r = 0; r < 8; r++) vs(r, o.view_proj[r]);
    for (int c = 0; c < 4; c++) o.depth_range[c] = g.F32(dev + kDev_PixelShaderF + 8 * 16 + c * 4);
    const uint32_t rows = o.bones * 3;
    o.rows.resize(size_t(rows) * 2 * 4);
    for (uint32_t i = 0; i < rows; i++) {
        vs(9 + i, &o.rows[size_t(i) * 4]);
        vs(129 + i, &o.rows[size_t(rows + i) * 4]);
    }
    if (s.velocity_meshes_frame != s.game_frame) {
        s.velocity_meshes.clear();
        s.velocity_meshes_frame = s.game_frame;
    }
    s.velocity_meshes.push_back({mesh, o.cull});
    fc.velocity_objects.push_back(std::move(o));
}

// A world frame's object pass (even/odd rendering: its world's velocity
// buffer is drawn by the next frame, which the live view's world frame stands
// in for), as the next frame's DrawMesh will draw it: the meshes the last
// post frame drew there that this frame's world drew too, each by the
// palettes the velocity buffer's caches hold for it (this frame's, which
// DxMesh::SetTransforms cached as the world drew, and the last world's), the
// camera's view-projection and the previous one (PostParams', as Draw will
// upload them) and its depth range, into `out`; one the caches lack is left
// out, as GetXfms leaves it. On frames that draw it, what this gives matched
// what DrawMesh drew exactly (render_song.b3t's captures).
void EmulateVelocityObjects(const Guest& g, State& s, FrameCapture& fc,
                            std::vector<VelocityObject>& out) {
    const PostParams& p = fc.post;
    if (!VelocityExpected(p) || s.velocity_meshes.empty()) return;
    constexpr uint32_t vb = kVelocityBuffer;
    const uint32_t idx = g.U32(vb + kVel_Index) & 1;
    const uint32_t caches[2] = {vb + kVel_XfmCaches + idx * kXfmCache_Size,
                                vb + kVel_XfmCaches + (idx ^ 1) * kXfmCache_Size};
    for (const State::VelocityMesh& m : s.velocity_meshes) {
        bool drawn = false;
        for (const DrawItem& d : fc.draws) drawn |= d.mesh == m.mesh && d.draw_mode == 0;
        if (!drawn) continue;
        uint32_t owner = g.U32(m.mesh + kMesh_GeomOwner);
        if (!owner) owner = m.mesh;
        const uint32_t mat = g.U32(owner + kMesh_Mat);
        if (!mat || g.U32(mat + kMat_ZMode) == 2) continue;
        const uint32_t bones = g.U32(owner + kMesh_BonesBegin);
        const uint32_t bones_end = g.U32(owner + kMesh_BonesEnd);
        const uint32_t n = bones && bones_end > bones ? (bones_end - bones) / kBone_Size : 0;
        if (n > kMaxVelocityBones) continue;
        const uint32_t count = std::max(n, 1u);
        // this frame's palette and the last's, where the caches have them
        uint32_t at[2] = {};
        bool cached = true;
        for (int k = 0; k < 2 && cached; k++) {
            const uint32_t key = g.U32(owner + kMesh_MotionKeys + 4 * (k ? idx ^ 1 : idx));
            const uint32_t c = caches[k];
            cached = key + count <= g.U32(c + kXfmCache_Used) && g.U32(c + key * 4) == owner &&
                     g.U32(c + (key + count - 1) * 4) == owner;
            at[k] = c + kXfmCache_Floats + key * 48;
        }
        if (!cached) continue;
        std::shared_ptr<const Geometry> geometry = CaptureGeometry(g, owner, fc);
        if (!geometry || !HasFaces(*geometry)) continue;
        VelocityObject o;
        o.geom = std::move(geometry);
        o.mesh = m.mesh;
        o.cull = m.cull;
        o.skinned = n > 0;
        o.bones = count;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                o.view_proj[r][c] = p.vel_view_proj[c][r];
                o.view_proj[4 + r][c] = p.vel_prev_view_proj[c][r];
            }
            o.depth_range[r] = p.vel_depth_range[r];
        }
        o.rows.resize(size_t(count) * 3 * 2 * 4);
        for (int k = 0; k < 2; k++)
            for (uint32_t i = 0; i < count * 12; i++)
                o.rows[size_t(k) * count * 12 + i] = g.F32(at[k] + i * 4);
        out.push_back(std::move(o));
    }
}

void CaptureMesh(uint8_t* base, uint32_t mesh, uint32_t mat, uint32_t pass, bool drawn = true) {
    State& s = S();
    const Guest g{base};
    // the velocity buffer's object pass, which is no draw of the frame's
    if (drawn) {
        const uint32_t holder = g.U32(kDrawModeHolder);
        if (holder && g.U32(holder + kDrawMode) == kDrawModeVelocity)
            CaptureVelocityObject(g, s, mesh);
    }
    std::optional<Sink> sink;
    if (!Target(g, s, sink)) return;
    Lap(s, CaptureProfile::kStepTarget);
    if (sink->target && s.open.rec && s.open.rec->pass.tex_type == kTexTypeDepthVolume &&
        g_shader_type == kDepthVolumeShader) {
        CaptureSpotCone(g, s, *sink, mesh);
        return;
    }
    std::shared_ptr<const Geometry> geometry;
    bool default_mat = false;
    if (!MeshParts(g, sink->fc, mesh, mat, default_mat, geometry)) {
        if (default_mat && DiagLog(s.logged_no_mat)) LogNoMaterialNotDrawn(g, s, mesh, drawn);
        return;
    }
    if (pass) sink->fc.later_passes++;

    DrawItem item =
        MakeItem(g, s, *sink, mat, mesh, std::move(geometry), true, false, default_mat);
    item.world = ReadXfm(g, mesh + kMesh_WorldXfm);
    item.cull = CaptureCull(g);
    const uint32_t bones = g.U32(mesh + kMesh_BonesBegin);
    const uint32_t bones_end = g.U32(mesh + kMesh_BonesEnd);
    Lap(s, CaptureProfile::kStepItem);
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
        s.profile.bones += n;
        Lap(s, CaptureProfile::kStepBones);
    }
    if (default_mat && DiagLog(s.logged_no_mat))
        LogNoMaterial(g, s, *sink, item, "a mesh's pass");
    PushDraw(s, *sink, std::move(item));
}

// DxMultiMesh draws its mesh once per instance, instanced, without going
// through the mesh's DrawShowing; one draw per instance here, for one
// material pass (`mat`, and `pass` as CaptureMesh's): its
// DrawBatchedNewGfx loops over them as DrawShowing does, SelectConfig then
// its batches of instances
void CaptureMultiMesh(uint8_t* base, uint32_t multimesh, uint32_t mat, uint32_t pass) {
    State& s = S();
    const Guest g{base};
    const uint32_t mesh = g.U32(multimesh + kMultiMesh_Mesh);
    std::optional<Sink> sink;
    if (!mesh || !Target(g, s, sink)) return;
    Lap(s, CaptureProfile::kStepTarget);
    std::shared_ptr<const Geometry> geometry;
    bool default_mat = false;
    if (!MeshParts(g, sink->fc, mesh, mat, default_mat, geometry)) return;
    if (pass) sink->fc.later_passes++;

    DrawItem proto =
        MakeItem(g, s, *sink, mat, mesh, std::move(geometry), true, false, default_mat);
    proto.cull = CaptureCull(g);
    if (default_mat && DiagLog(s.logged_no_mat))
        LogNoMaterial(g, s, *sink, proto, "a multimesh's pass");
    // std::list with its sentinel node inline: next at +0, the Instance at +8
    const uint32_t head = multimesh + kMultiMesh_Instances;
    uint32_t n = 0;
    for (uint32_t node = g.U32(head); node && node != head && n < kMaxInstances;
         node = g.U32(node), n++) {
        DrawItem item = proto;
        item.world = ReadXfm(g, node + 8);
        PushDraw(s, *sink, std::move(item));
    }
    sink->fc.multimesh_instances += n;
}

// A DxMesh::DrawFaces outside a DrawShowing (RndTexBlender's, the velocity
// buffer's), counted (faces_elsewhere) in the texture pass it draws into or
// the frame, and not recorded; the velocity buffer's aren't counted, as its
// DrawShowing's aren't recorded either. The first few are logged, with where
// they draw.
void CountFacesElsewhere(const Guest& g, uint32_t geom) {
    State& s = S();
    const uint32_t holder = g.U32(kDrawModeHolder);
    const uint32_t mode = holder ? g.U32(holder + kDrawMode) : kDrawModeNormal;
    if (mode == kDrawModeVelocity) return;
    FrameCapture* fc = nullptr;
    if (s.open.tex) {
        if (s.open.record) fc = &s.open.rec->content;
    } else if (g_enabled.load(std::memory_order_relaxed)) {
        fc = s.building.get();
    }
    if (fc) fc->faces_elsewhere++;
    static int logged = 0;
    if (logged < 8 || DiagLog(s.logged_elsewhere)) {
        logged++;
        REXLOG_INFO("native view: DxMesh::DrawFaces of {:08X} '{}' outside a DrawShowing, "
                    "material {:08X}, draw mode {}, into {:08X} ({}), camera {:08X}{}, frame {}",
                    geom, VirtualBaseName(g, geom), g_selected_mat, mode, s.open.tex,
                    s.open.tex && s.open.rec ? s.open.rec->pass.name : std::string(), s.cam,
                    s.cam_backbuffer ? " (back buffer)" : "", s.game_frame);
    }
}

// DxParticleSys's vertex fill: one quad per active particle, built as the
// particle VS builds it from the one vertex DrawParticles writes per particle
// (position, colour, size, angle, swing arm: ParticleCorner)
void CaptureParticles(uint8_t* base, uint32_t sys) {
    State& s = S();
    const Guest g{base};
    const uint32_t mat = g.U32(sys + kPart_Mat);
    std::optional<Sink> sink;
    if (!mat || !g.U32(sys + kPart_NumActive) || !s.cam || !Target(g, s, sink, true)) return;
    Lap(s, CaptureProfile::kStepTarget);

    // the camera's right (x) and up (z) axes; Milo cameras look down +y. The
    // quad's own are VS c47 and c48, which DxParticleSys::DrawShowing sets
    // before DrawParticles draws: the camera's axes as it transforms them,
    // scaled by half, the up axis by a float of the system's too (+0x2c0),
    // so c48 is 0.15..0.5 long where c47 is 0.5 in the draws seen. The
    // camera's halved without a device.
    const Mat4 cam = ReadXfm(g, s.cam + kTrans_WorldXfm);
    float right[3], up[3];
    for (int i = 0; i < 3; i++) {
        right[i] = cam.m[0][i] * 0.5f;
        up[i] = cam.m[2][i] * 0.5f;
    }
    if (const uint32_t dev = g.U32(kD3DDeviceHolder)) {
        for (int i = 0; i < 3; i++) {
            right[i] = g.F32(dev + kDev_VertexShaderF + 47 * 16 + i * 4);
            up[i] = g.F32(dev + kDev_VertexShaderF + 48 * 16 + i * 4);
        }
        // Not modelled, 0 in every draw seen: c49.x, set from the system's
        // byte +0x2b8, which also has DrawParticles write the particle's
        // velocity in place of its angle and swing arm, for the VS to stretch
        // the quad along it (instrs 29-60), and c49.z, which clamps the
        // stretched size on that path.
    }
    auto geom = std::make_shared<Geometry>();
    uint32_t n = 0;
    for (uint32_t p = g.U32(sys + kPart_Active); p && n < kMaxParticles;
         p = g.U32(p + kParticle_Next), n++) {
        float pos[3], col[4];
        for (int i = 0; i < 3; i++) pos[i] = g.F32(p + kParticle_Pos + i * 4);
        for (int i = 0; i < 4; i++) col[i] = g.F32(p + kParticle_Color + i * 4);
        const float size = g.F32(p + kParticle_Size);
        const float angle = g.F32(p + kParticle_Angle);
        const float swing = g.F32(p + kParticle_SwingArm);
        const uint32_t rgba = ParticleColor(col);  // unclamped, as the game packs it
        const uint16_t first = uint16_t(geom->verts.size());
        for (int k = 0; k < 4; k++) {
            Vertex v{};
            ParticleCorner(pos, right, up, size, angle, swing, k, v.pos, v.uv);
            for (int i = 0; i < 3; i++) v.nrm[i] = -cam.m[1][i];
            v.color = rgba;
            geom->verts.push_back(v);
        }
        geom->indices.insert(geom->indices.end(),
                             {first, uint16_t(first + 1), uint16_t(first + 2), first,
                              uint16_t(first + 2), uint16_t(first + 3)});
    }
    s.profile.allocs++;
    Lap(s, CaptureProfile::kStepParticleGeom);
    if (geom->indices.empty()) return;
    sink->fc.particles += n;
    DrawItem item = MakeItem(g, s, *sink, mat, sys, std::move(geom));
    item.prelit = true;  // the particle colour is the vertex colour
    PushDraw(s, *sink, std::move(item));
}

// DxRnd::DrawRect(this, Hmx::Rect* px, RndMat* mat, ShaderType, Color* color,
// Color* color1, Color* color2) (band3_recomp.31.cpp; out/research/
// m3_survey.md 3): a quad over the rect (x, y, w, h in the bound target's
// pixels), coloured by the material's colour unless it's prelit or there's
// none, else by `color`, or a gradient: from `color` on the left to color1 on
// the right if there's a color1, else from the top to color2 at the bottom if
// there's a color2 (rb3-xenon rnddx9/Rnd.cpp). Its uv is 0..1
// from the top left, through the material's texture transform when its tex
// gen is kTexGenXfmOrigin, as DrawRect works it out on the CPU (rb3-xenon
// rnddx9/Rnd.cpp): u' = m.x.x u - m.y.x v + v.x, v' = m.y.y v - m.x.y u + v.y,
// MakeTex3's matrix about the origin. The shader's texgen (VS c20/c21, which
// the shade state keeps) transforms it again, as the game's VS does.
void CaptureRect(uint8_t* base, uint32_t rnd, uint32_t rect_ptr, uint32_t mat, int32_t shader,
                 uint32_t color_ptr, uint32_t color1_ptr, uint32_t color2_ptr) {
    State& s = S();
    const Guest g{base};
    std::optional<Sink> sink;
    if (!rect_ptr || !Target(g, s, sink, false, true)) return;
    Lap(s, CaptureProfile::kStepTarget);

    // the bound target's size: the texture's, or its mip level's while
    // FinishDrawTarget builds them; else the screen's
    float tw, th;
    int32_t mip = 0;
    if (sink->target) {
        const Pass& p = s.open.rec->pass;
        if (s.open.in_finish) mip = ++s.open.mips_drawn;
        tw = float(std::max(1u, p.width >> mip));
        th = float(std::max(1u, p.height >> mip));
    } else {
        tw = float(g.U32(rnd + kRnd_Width));
        th = float(g.U32(rnd + kRnd_Height));
    }
    if (tw <= 0 || th <= 0) return;
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = g.F32(rect_ptr + i * 4);

    auto pack = [&](uint32_t color) {
        float col[4] = {1, 1, 1, 1};
        if (color)
            for (int i = 0; i < 4; i++) col[i] = g.F32(color + i * 4);
        uint32_t rgba = 0;
        for (int i = 0; i < 4; i++)
            rgba |= uint32_t(std::clamp(col[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
        return rgba;
    };
    // the corners' colours, in `corner`'s order: top left, top right, bottom
    // right, bottom left
    uint32_t rgba[4];
    if (mat && !g.U8(mat + kMat_Prelit)) {
        std::fill(std::begin(rgba), std::end(rgba), pack(mat + kMat_Color));
    } else {
        const uint32_t c = pack(color_ptr);
        std::fill(std::begin(rgba), std::end(rgba), c);
        if (color1_ptr) {
            rgba[1] = rgba[2] = pack(color1_ptr);
        } else if (color2_ptr) {
            rgba[2] = rgba[3] = pack(color2_ptr);
        }
    }
    // the texture transform's rows x and y and its translation, identity
    // without one
    float xx = 1, xy = 0, yx = 0, yy = 1, tx = 0, ty = 0;
    if (mat && g.U32(mat + kMat_TexGen) == kTexGenXfmOrigin) {
        xx = g.F32(mat + kMat_TexXfm + 0x00);
        xy = g.F32(mat + kMat_TexXfm + 0x04);
        yx = g.F32(mat + kMat_TexXfm + 0x10);
        yy = g.F32(mat + kMat_TexXfm + 0x14);
        tx = g.F32(mat + kMat_TexXfm + 0x30);
        ty = g.F32(mat + kMat_TexXfm + 0x34);
    }
    auto geom = std::make_shared<Geometry>();
    const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; k++) {
        const float* c = corner[k];
        Vertex v{};
        v.pos[0] = (r[0] + c[0] * r[2]) / tw * 2.0f - 1.0f;
        v.pos[1] = 1.0f - (r[1] + c[1] * r[3]) / th * 2.0f;
        v.nrm[2] = -1.0f;
        v.uv[0] = xx * c[0] - yx * c[1] + tx;
        v.uv[1] = yy * c[1] - xy * c[0] + ty;
        v.color = rgba[k];
        geom->verts.push_back(v);
    }
    geom->indices = {0, 1, 2, 0, 2, 3};
    s.profile.allocs++;
    Lap(s, CaptureProfile::kStepRectGeom);

    DrawItem item;
    if (mat) {
        item = MakeItem(g, s, *sink, mat, mat, std::move(geom), mip == 0,
                        shader == kRectShaderBlur);
        if (mip) item.shade = -1;
    } else {
        item.geom = std::move(geom);
        for (float& c : item.color) c = 1.0f;
        item.blend = 1;   // kBlendSrc
        item.z_mode = 0;  // kZModeDisable
        item.alpha_cut = false;
        item.alpha_threshold = 0;
        item.cam = s.cam;
        item.mesh = 0;
        item.target = sink->target;
    }
    item.world = Identity();
    item.view_proj = Identity();
    item.prelit = true;  // its colour is the vertex colour
    item.rect_shader = shader;
    for (int i = 0; i < 4; i++) item.rect[i] = r[i];
    item.mip_level = mip;
    PushDraw(s, *sink, std::move(item));
}

// ---------------------------------------------------------------------------
// texture passes

// a guest C string, up to kMaxName characters
std::string ReadName(const Guest& g, uint32_t p) {
    std::string out;
    if (p < 0x10000u || p >= 0xF0000000u) return out;
    for (uint32_t i = 0; i < kMaxName; i++) {
        const uint8_t c = g.U8(p + i);
        if (!c) break;
        out += char(c);
    }
    return out;
}

void DropOpenPass(State& s) {
    if (s.open.tex) s.building->passes_unbalanced++;
    s.open = OpenPass{};
    g_pass_recording.store(false, std::memory_order_relaxed);
    SetPassWantsDraws(false);
}

// The most frames apart even/odd rendering draws the world now: 2 at 30 fps
// on the console's 60 Hz, 3 at 20, 4 at 30 at refresh_rate 120
// (frame_pacing.h); at least 2.
uint64_t WorldPeriod() { return std::max<uint64_t>(2, band3::pacing::WorldPeriod()); }

// a world frame is composed with the frames after it that draw none for up to
// a period of frames (the last of them is a period less one after it)
uint64_t MaxWorldAge() { return WorldPeriod(); }

// a texture drawn into again within a period of frames is drawn regularly:
// every frame, or every world frame with even/odd rendering on
uint64_t RepeatFrames() { return WorldPeriod(); }

bool RecentlyMade(const RtState& rt, uint64_t frame) {
    return rt.made_frame != ~0ull && rt.made_frame + RepeatFrames() >= frame;
}

// DxTex::MakeDrawTarget: the game starts drawing into `tex`. While capture is
// off, a texture drawn into regularly isn't recorded once it has been twice in
// a row: the crowd's impostor, blurs and the rest are drawn again every frame
// (or world frame), and a capture records them itself then. One drawn once, or
// twice (made, then made again), is. The same rule keeps the emulated GPU
// drawing a pass while it skips the game's draws (gpu_skip.h): one that isn't
// regular is drawn, so what RB3 makes once is there after F8 back.
void BeginPass(const Guest& g, uint32_t tex) {
    State& s = S();
    // the same texture again is its camera selected again, which clears it:
    // the pass starts over
    if (s.open.tex == tex) s.open = OpenPass{};
    if (s.open.tex) DropOpenPass(s);
    const bool capturing = g_enabled.load(std::memory_order_relaxed);
    if (!capturing) g_rec_passes.fetch_add(1, std::memory_order_relaxed);
    RtState& rt = s.rts[tex];
    rt.base = TexBase(g, tex);
    const bool regular = RecentlyMade(rt, s.game_frame) && rt.repeats >= 1;
    SetPassWantsDraws(!regular);
    s.open.tex = tex;
    s.open.record = capturing || !regular;
    g_pass_recording.store(s.open.record, std::memory_order_relaxed);
    if (!s.open.record) return;
    if (!capturing) g_rec_recorded.fetch_add(1, std::memory_order_relaxed);
    s.open.rec = std::make_shared<PassRecord>();
    s.profile.allocs++;
    // as much as its last pass drew
    if (rt.last) {
        s.open.rec->content.draws.reserve(rt.last->content.draws.size());
        s.open.rec->content.shades.reserve(rt.last->content.shades.size());
    }
    Pass& p = s.open.rec->pass;
    p.tex_obj = tex;
    p.width = g.U32(tex + kTex_Width);
    p.height = g.U32(tex + kTex_Height);
    p.tex_type = g.U32(tex + kTex_Type);
    p.num_mips = g.U32(tex + kTex_NumMips);
    p.format = g.U32(tex + kDxTex_Format);
    p.from_frame = s.game_frame;
    p.name = ReadName(g, g.U32(tex + kObj_Name));
}

// DxCam::Select, after it has bound its camera's target and cleared it
// (band3_recomp.178.cpp: colour unless a shadow map, to 0, or opaque black for
// a depth volume; depth and stencil when it has a depth buffer, to the
// image's constants)
void CameraSelected(const Guest& g, uint32_t cam) {
    State& s = S();
    if (!s.open.tex || !s.open.record || g.U32(cam + kCam_TargetTex) != s.open.tex) return;
    PassRecord& rec = *s.open.rec;
    Pass& p = rec.pass;
    const uint32_t type = p.tex_type;
    p.cam = cam;
    p.clear_flags = 0;
    if ((type & 2) && !(type & kTexType_NoZ)) p.clear_flags |= 0x30;
    if (type != kTexTypeShadowMap) p.clear_flags |= 0x0f;
    p.clear_color = type == kTexTypeDepthVolume ? 0xFF000000u : 0;
    p.clear_z = g.F32(type == kTexTypeShadowMap ? kClearDepthShadow : kClearDepth);
    const float size[4] = {float(p.width), float(p.height), float(p.width), float(p.height)};
    for (int i = 0; i < 4; i++) p.viewport[i] = g.F32(cam + kCam_ScreenRect + i * 4) * size[i];
    // a select clears what was drawn so far
    rec.content.draws.clear();
    rec.samples.clear();
}

// A back-buffer camera RndCam::Select just selected, into the frame's
// cameras (scene_capture.h's CameraView): its viewport as DxCam::SetViewport
// works it out (rb3-xenon rnddx9/Cam.cpp; TheHiResScreen is never active) and
// its z range. One selected again keeps what it has now.
void RecordCamera(const Guest& g, State& s, uint32_t cam) {
    const uint32_t rnd = g.U32(kDrawModeHolder);
    if (!rnd) return;
    CameraView v;
    v.cam = cam;
    v.target_w = g.U32(rnd + kRnd_Width);
    v.target_h = g.U32(rnd + kRnd_Height);
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = g.F32(cam + kCam_ScreenRect + i * 4);
    const float x = std::min(1.0f, std::max(0.0f, r[0]));
    const float y = std::min(1.0f, std::max(0.0f, r[1]));
    const float x2 = std::min(1.0f, std::max(0.0f, r[0] + r[2]));
    const float y2 = std::min(1.0f, std::max(0.0f, r[1] + r[3]));
    const float width = float(v.target_w), height = float(v.target_h);
    v.viewport[0] = float(uint32_t(width * x));
    v.viewport[1] = float(uint32_t(height * y));
    v.viewport[2] = float(uint32_t(width * std::max(0.0f, x2 - x)));
    v.viewport[3] = float(uint32_t(height * std::max(0.0f, y2 - y)));
    v.zrange[0] = g.F32(cam + kCam_ZRange);
    v.zrange[1] = g.F32(cam + kCam_ZRange + 4);
    std::vector<CameraView>& cams = s.building->cameras;
    for (CameraView& c : cams) {
        if (c.cam != cam) continue;
        c = v;
        return;
    }
    cams.push_back(v);
}

// `rec`'s draws, shades and samples to the end of `fc`
void AppendPass(State& s, FrameCapture& fc, const PassRecord& rec) {
    std::vector<int32_t> remap(rec.content.shades.size());
    for (size_t i = 0; i < remap.size(); i++) {
        ShadeState copy = rec.content.shades[i];
        remap[i] = InternShade(fc, s.shades, std::move(copy), [](ShadeState&) {});
    }
    Pass p = rec.pass;
    p.first_draw = uint32_t(fc.draws.size());
    p.draw_count = uint32_t(rec.content.draws.size());
    for (const DrawItem& d : rec.content.draws) {
        DrawItem c = d;
        if (c.shade >= 0) c.shade = remap[c.shade];
        fc.draws.push_back(std::move(c));
    }
    fc.passes.push_back(std::move(p));
    s.samples.insert(s.samples.end(), rec.samples.begin(), rec.samples.end());
    Lap(s, CaptureProfile::kStepPassAppend);
}

void AddCounts(FrameCapture& to, const FrameCapture& from) {
    to.skipped_velocity += from.skipped_velocity;
    to.skipped_shadow += from.skipped_shadow;
    to.skipped_draw_mode += from.skipped_draw_mode;
    to.skipped_no_geom += from.skipped_no_geom;
    to.mutable_meshes += from.mutable_meshes;
    to.multimesh_instances += from.multimesh_instances;
    to.particles += from.particles;
    to.textured += from.textured;
    to.untextured_format += from.untextured_format;
    to.geom_cached += from.geom_cached;
    to.tex_cached += from.tex_cached;
    to.maps_decoded += from.maps_decoded;
    to.maps_cube += from.maps_cube;
    to.maps_other_format += from.maps_other_format;
    to.rt_snapshots += from.rt_snapshots;
    to.later_passes += from.later_passes;
    to.skipped_no_mat += from.skipped_no_mat;
    to.faces_elsewhere += from.faces_elsewhere;
}

// whether a pass drew nothing the capture keeps because every draw it made
// was left out: for its draw mode (velocity), or for having no material or
// geometry the capture draws
bool AllLeftOut(const FrameCapture& content) {
    const uint32_t left_out = content.skipped_shadow + content.skipped_velocity +
                              content.skipped_draw_mode + content.skipped_no_geom;
    return content.draws.empty() && left_out > 0;
}

// DxTex::FinishDrawTarget: `tex` is resolved, a new version of it. A recorded
// pass becomes its last, and the capturing frame's next pass; one that drew
// nothing kept (the velocity buffer's) is left out, though one whose draws
// were all left out is still its last, so a capture sampling it counts it as
// such (rt_filtered) rather than missing. One whose camera cleared it and
// that drew nothing else is kept with no draws: the clear is what it made
// (NgSpotlightDrawer clears the depth volume to opaque black with no cone in
// view, and blurs it after; without the clear the blurs would blur whatever
// the target held before).
void EndPass(uint32_t tex) {
    State& s = S();
    SetPassWantsDraws(false);
    if (s.open.tex != tex) DropOpenPass(s);
    RtState& rt = s.rts[tex];
    rt.version++;
    rt.repeats = RecentlyMade(rt, s.game_frame) ? rt.repeats + 1 : 0;
    rt.made_frame = s.game_frame;
    rt.before = std::move(rt.last);
    if (!s.open.tex) return;
    const bool capturing = g_enabled.load(std::memory_order_relaxed);
    std::shared_ptr<PassRecord> rec = std::move(s.open.rec);
    const bool recorded = s.open.record;
    s.open = OpenPass{};
    g_pass_recording.store(false, std::memory_order_relaxed);
    if (!recorded) return;
    rec->pass.version = rt.version;
    rec->pass.draw_count = uint32_t(rec->content.draws.size());
    // what it left out (velocity) counts in the frame either way
    if (capturing) AddCounts(*s.building, rec->content);
    const bool clear_only = rec->content.draws.empty() && rec->pass.cam &&
                            (rec->pass.clear_flags & 0x0f) && !AllLeftOut(rec->content);
    if (rec->content.draws.empty() && !clear_only) {
        if (capturing) s.building->passes_empty++;
        if (AllLeftOut(rec->content)) {
            // kept for the frame too: later passes into the same texture can
            // push it out of last and before (NgLight's shadow, then its blurs)
            if (capturing) s.left_out.insert(RtKey(tex, rt.version));
            rt.last = std::move(rec);
        }
        return;
    }
    rt.last = rec;
    if (capturing) {
        AppendPass(s, *s.building, *rec);
        s.building->passes_own++;
    }
}

// a texture gone, or made again: its passes and versions with it
void ForgetTexture(uint32_t tex) {
    State& s = S();
    if (s.open.tex == tex) DropOpenPass(s);
    s.rts.erase(tex);
}

// Passes in front of the frame's own for the render targets it samples that
// it didn't draw itself, from their textures' last passes; and what's
// sampled, missing and made by a pass whose draws were all left out counted.
void CarryPasses(State& s, FrameCapture& fc) {
    std::unordered_set<uint64_t> have;
    for (const Pass& p : fc.passes)
        if (p.tex_obj) have.insert(RtKey(p.tex_obj, p.version));
    std::vector<uint64_t> todo;
    std::unordered_set<uint64_t> seen;
    for (uint64_t k : s.samples)
        if (seen.insert(k).second) todo.push_back(k);
    std::vector<const PassRecord*> carried;
    for (size_t i = 0; i < todo.size(); i++) {
        const uint64_t key = todo[i];
        if (have.count(key)) continue;
        if (s.left_out.count(key)) {
            fc.rt_filtered++;
            fc.rt_filtered_keys.push_back(key);
            continue;
        }
        const PassRecord* found = nullptr;
        if (auto it = s.rts.find(uint32_t(key >> 32)); it != s.rts.end()) {
            for (const PassRecord* rec : {it->second.last.get(), it->second.before.get()})
                if (rec && rec->pass.version == uint32_t(key)) found = rec;
        }
        if (!found) {
            fc.rt_missing++;
            continue;
        }
        // (one that only cleared is carried: its clear is what it made)
        if (AllLeftOut(found->content)) {
            fc.rt_filtered++;
            fc.rt_filtered_keys.push_back(key);
            continue;
        }
        carried.push_back(found);
        have.insert(key);
        // and what it sampled in turn
        for (uint64_t k : found->samples)
            if (seen.insert(k).second) todo.push_back(k);
    }
    fc.rt_sampled = uint32_t(seen.size());
    Lap(s, CaptureProfile::kStepCarry);
    if (carried.empty()) return;

    std::vector<DrawItem> own_draws = std::move(fc.draws);
    std::vector<Pass> own_passes = std::move(fc.passes);
    fc.draws.clear();
    fc.passes.clear();
    size_t draws = own_draws.size(), shades = fc.shades.size();
    for (const PassRecord* rec : carried) {
        draws += rec->content.draws.size();
        shades += rec->content.shades.size();
    }
    fc.draws.reserve(draws);
    fc.passes.reserve(own_passes.size() + carried.size());
    fc.shades.reserve(shades);
    // what a carried pass samples was found after it, so goes before it
    for (auto it = carried.rbegin(); it != carried.rend(); ++it) AppendPass(s, fc, **it);
    fc.passes_carried = uint32_t(carried.size());
    const uint32_t shift = uint32_t(fc.draws.size());
    for (Pass& p : own_passes) {
        p.first_draw += shift;
        fc.passes.push_back(std::move(p));
    }
    for (DrawItem& d : own_draws) fc.draws.push_back(std::move(d));
    if (fc.post_boundary != FrameCapture::kNoPost) fc.post_boundary += shift;
    Lap(s, CaptureProfile::kStepCarry);
}

// What post-processing is set to do, at DxRnd::DoPostProcess: the proc
// Rnd::DoPostProcess runs (TheRnd's override, alone, else the current one),
// TheDOFProc and TheRnd's copy of the world camera. Every pointer may be null
// (menus without a proc, a frame before the first world).
void ReadPostParams(const Guest& g, PostParams& p) {
    p = PostParams{};
    const uint32_t rnd = g.U32(kDrawModeHolder);
    if (!rnd) return;
    p.valid = 1;
    p.disabled = g.U8(rnd + kRnd_DisablePostProc);
    const uint32_t over = g.U32(rnd + kRnd_PostProcOverride);
    p.overridden = over != 0;
    p.proc = over ? over - kPostProcessor : g.U32(kPostProcCurrent);
    if (const uint32_t proc = p.proc) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) p.xfm[r][c] = g.F32(proc + kPostProc_Xfm + r * 0x10 + c * 4);
            p.xfm_offset[r] = g.F32(proc + kPostProc_Xfm + 0x30 + r * 4);
        }
        p.color_mod = g.F32(proc + kPostProc_ColorMod);
        float* params[] = {&p.hue, &p.saturation, &p.lightness, &p.contrast, &p.brightness};
        for (int i = 0; i < 5; i++) *params[i] = g.F32(proc + kPostProc_Hue + i * 4);
        for (int c = 0; c < 4; c++) {
            p.level_in_lo[c] = g.F32(proc + kPostProc_LevelInLo + c * 4);
            p.level_in_hi[c] = g.F32(proc + kPostProc_LevelInHi + c * 4);
            p.level_out_lo[c] = g.F32(proc + kPostProc_LevelOutLo + c * 4);
            p.level_out_hi[c] = g.F32(proc + kPostProc_LevelOutHi + c * 4);
            p.bloom_color[c] = g.F32(proc + kPostProc_BloomColor + c * 4);
        }
        p.bloom_threshold = g.F32(proc + kPostProc_BloomThreshold);
        p.bloom_intensity = g.F32(proc + kPostProc_BloomIntensity);
        p.bloom_glare = g.U8(proc + kPostProc_BloomGlare);
        p.bloom_streak = g.U8(proc + kPostProc_BloomStreak);
        p.emulate_fps = g.F32(proc + kPostProc_EmulateFps);
        p.noise_base[0] = g.F32(proc + kPostProc_NoiseBase);
        p.noise_base[1] = g.F32(proc + kPostProc_NoiseBase + 4);
        p.noise_top = g.F32(proc + kPostProc_NoiseTop);
        p.noise_intensity = g.F32(proc + kPostProc_NoiseIntensity);
        p.noise_stationary = g.U8(proc + kPostProc_NoiseStationary);
        p.noise_midtone = g.U8(proc + kPostProc_NoiseMidtone);
        p.noise_map = g.U32(proc + kPostProc_NoiseMap);
        p.noise_map_base = TexBase(g, p.noise_map);
        p.noise_seeds[0] = g.F32(proc + kPostProc_NoiseSeeds);
        p.noise_seeds[1] = g.F32(proc + kPostProc_NoiseSeeds + 4);
        p.trail_threshold = g.F32(proc + kPostProc_TrailThreshold);
        p.trail_duration = g.F32(proc + kPostProc_TrailDuration);
    }
    if (const uint32_t dof = g.U32(kDOFProcHolder)) {
        p.dof = dof;
        p.dof_enabled = g.U8(dof + kDOF_Enabled);
        float* fields[] = {&p.dof_scale,      &p.dof_bias,     &p.dof_focal,
                           &p.dof_blur_depth, &p.dof_min_blur, &p.dof_max_blur};
        for (int i = 0; i < 6; i++) *fields[i] = g.F32(dof + kDOF_Scale + i * 4);
    }
    p.blur_width_scale = g.F32(kDOFOverride_BlurWidthScale);
    if (const uint32_t cam = g.U32(rnd + kRnd_WorldCam)) {
        p.cam = cam;
        p.cam_near = g.F32(cam + kCam_Near);
        p.cam_far = g.F32(cam + kCam_Far);
        p.cam_zrange[0] = g.F32(cam + kCam_ZRange);
        p.cam_zrange[1] = g.F32(cam + kCam_ZRange + 4);
    }
    // The velocity buffer, as Draw will find it: EndWorld's
    // CacheCameraSettings (in DoWorldEnd, before this) set the camera's
    // matrix and frustum, and Draw keeps the matrix at index idx and reads
    // the previous one at idx ^ 1 before AdvanceFrame flips idx
    constexpr uint32_t vb = kVelocityBuffer;
    if (g.U32(vb + kVel_Tex)) {
        p.vel_read = 1;
        p.vel_on = p.proc ? g.U8(p.proc + kPostProc_MotionBlurVelocity) != 0 : 0;
        const uint32_t ng = g.U32(kNgRndHolder);
        p.vel_pre_depth = ng && g.U32(ng + kNgRnd_PreDepth) != 0;
        const uint32_t world = g.U32(rnd + kRnd_WorldCam);
        p.vel_same_cam = world && g.U32(vb + kVel_Cam) == world && g.U32(vb + kVel_LastCam) == world;
        p.vel_frame = g.U32(vb + kVel_Frame);
        p.vel_scale = g.F32(vb + kVel_Scale);
        const uint32_t prev = vb + kVel_Xfms + 64 * ((g.U32(vb + kVel_Index) & 1) ^ 1);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                p.vel_view_proj[r][c] = g.F32(vb + kVel_ViewProj + r * 16 + c * 4);
                p.vel_prev_view_proj[r][c] = g.F32(prev + r * 16 + c * 4);
                p.vel_corners[r][c] = g.F32(vb + kVel_FrustumCorners + r * 16 + c * 4);
            }
            p.vel_depth_range[r] = g.F32(vb + kVel_DepthRange + r * 4);
        }
        for (int c = 0; c < 3; c++) p.vel_near[c] = g.F32(vb + kVel_FrustumNear + c * 4);
    }
}

// pixel shader constant `reg` from the device's shadow
void ReadPsConst(const Guest& g, uint32_t dev, int reg, float out[4]) {
    for (int c = 0; c < 4; c++) out[c] = g.F32(dev + kDev_PixelShaderF + reg * 16 + c * 4);
}

// What the composite is about to draw with, at DxRnd::FinishPostProcess
void ReadPostConsts(const Guest& g, PostConsts& pc) {
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return;
    pc.valid = 1;
    ReadPsConst(g, dev, 6, pc.c6);
    ReadPsConst(g, dev, 15, pc.c15);
    ReadPsConst(g, dev, 24, pc.c24);
    ReadPsConst(g, dev, 91, pc.c91);
    for (int r = 0; r < 3; r++) ReadPsConst(g, dev, 92 + r, pc.c92[r]);
    ReadPsConst(g, dev, 112, pc.c112);
    ReadPsConst(g, dev, 113, pc.c113);
    ReadPsConst(g, dev, 122, pc.c122);
    ReadPsConst(g, dev, 125, pc.c125);
    ReadPsConst(g, dev, 127, pc.c127);
    // the velocity pass's, as RndVelocityBuffer::Draw left them (replay's
    // "velocity:" line sets them against PostParams' vel_*), and the samplers
    // the composite blurs the scene by
    ReadPsConst(g, dev, 89, pc.c89);
    for (int r = 0; r < 4; r++) ReadPsConst(g, dev, 134 + r, pc.c134[r]);
    for (int i = 0; i < 6; i++) {
        pc.scene_fetch[i] = g.U32(dev + kDev_TextureFetch + kSceneSampler * 24 + i * 4);
        pc.velocity_fetch[i] = g.U32(dev + kDev_TextureFetch + kVelocitySampler * 24 + i * 4);
    }
    if (const uint32_t sm = g.U32(kShaderMgrHolder)) {
        for (int i = 0; i < int(sizeof(pc.flags)); i++) pc.flags[i] = g.U8(sm + kPostFlagBase + i);
        pc.spot_flag = g.U8(sm + kPostFlagSpot);
    }
}

// The noise map the composite reads, at FinishPostProcess where
// TheShaderMgr + 0x2D says the noise is on: sampler 13's fetch constant, as
// NgPostProc::CheckNoise bound it (the map, its filter linear and its
// addressing wrap), and its pixels and mips. A static texture, so decoded
// once and found in the cache after (deferred, if a shade's map found it
// first: decoded with the frame, deferred_decode.h). Kept for the world
// frames after it.
void CaptureNoise(const Guest& g, FrameCapture& fc) {
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return;
    PostConsts& pc = fc.post_consts;
    for (int i = 0; i < 6; i++)
        pc.noise_fetch[i] = g.U32(dev + kDev_TextureFetch + kNoiseSampler * 24 + i * 4);
    const int32_t aniso = g_aniso_override.load(std::memory_order_relaxed);
    std::shared_ptr<const Texture> tex;
    TexSampler sampler;
    if (const uint32_t base = pc.noise_fetch[1] & 0xfffff000u) {
        tex = DecodeCached(g, pc.noise_fetch, base, S().map_texs, fc);
        sampler = guest_format::DecodeSampler(pc.noise_fetch, aniso);
    } else if (fc.post.noise_map) {
        // nothing bound there (none seen): the proc's map, by its own fetch
        // constant, with the filter and addressing CheckNoise sets
        tex = GuestPixels(g, fc.post.noise_map, fc);
        if (const uint32_t d3d = g.U32(fc.post.noise_map + kDxTex_Texture)) {
            uint32_t f[6];
            for (int i = 0; i < 6; i++) f[i] = g.U32(d3d + kD3DBaseTexture_Fetch + i * 4);
            sampler = guest_format::DecodeSampler(f, aniso);
            sampler.mag_linear = sampler.min_linear = 1;
            sampler.clamp_x = sampler.clamp_y = 0;
        }
    }
    if (!tex || !HasPixels(*tex)) return;
    fc.noise_map = tex;
    fc.noise_sampler = sampler;
    State& s = S();
    s.noise_map = tex;
    s.noise_sampler = sampler;
    s.noise_base = fc.post.noise_map_base;
}

// a blur's taps, c31.. (offsets) and c47.. (weights), after it set them
template <int N>
void ReadBlurTaps(const Guest& g, float offsets[N][4], float weights[N][4]) {
    const uint32_t dev = g.U32(kD3DDeviceHolder);
    if (!dev) return;
    for (int i = 0; i < N; i++) {
        ReadPsConst(g, dev, 31 + i, offsets[i]);
        ReadPsConst(g, dev, 47 + i, weights[i]);
    }
}

// A request takes the next frame whose capture shows the world the game's
// picture of it does: with even/odd rendering a post frame, composed with the
// world frame before it, and without, any (frame_compose.h). A frame that
// doesn't say what it drew (no DoPostProcess: some menus', and builds from
// before) is taken the old way: RB3 alternates frames that draw the scene
// with ones that only redraw the overlay (a handful of draws, sometimes from
// two cameras), so the request learns what a full frame draws over a couple
// of frames, then takes the next one that draws about as much.
void HoldIfRequested(const std::shared_ptr<const FrameCapture>& frame) {
    std::unique_lock lock(g_held.mutex);
    if (!g_held.armed) return;
    // While the emulated GPU skips the game's draws (gpu_skip.h) its picture
    // isn't this frame's: neither taken nor counted until it has drawn
    // kWholeFramesToHold frames whole in a row, this one and the ones before
    // (what it builds up over frames back too, not just the frame), which
    // the harness's capture asks for (RequestFullFrames) before it holds one.
    // With renderer native there's no emulated picture to wait for.
    if (!sync_gpu::NativeOnly() && EmulatedWholeFrames() < kWholeFramesToHold) return;
    if (g_held.skip > 0) {
        g_held.skip--;
        return;
    }
    const size_t draws = frame->draws.size();
    bool take;
    if (ProcKnown(*frame)) {
        take = PresentsCapturedWorld(*frame);
    } else {
        const bool learning = g_held.waited < kFramesToLearn;
        take = !learning && draws > 0 && draws * 10 >= g_held.most * 9;
    }
    g_held.most = std::max(g_held.most, draws);
    if (!take && ++g_held.waited <= kMaxFramesToWait) return;
    g_held.armed = false;
    g_held.frame = frame;
    g_held.fell_back = !take;
    g_held.released = false;
    g_held.cv.notify_all();
    g_held.cv.wait_for(lock, kMaxHold, [] { return g_held.released; });
    g_held.released = true;
}

// whether each channel of the table never falls as the input rises, as any
// gamma curve's doesn't: what tells a ramp read through the SDK's headers
// from one read at the wrong place
bool PlausibleTable(const GammaRamp& g) {
    for (int c = 0; c < 3; c++)
        for (int v = 1; v < 256; v++)
            if (TableChannel(g.table[v], c) < TableChannel(g.table[v - 1], c)) return false;
    return TableChannel(g.table[255], 0) != 0;
}

// The command processor's ramps, which its header keeps for its backends
// (protected): reached through a pointer to the member, as a class derived
// from it may take one. Never made.
struct GammaRampAccess : rex::graphics::CommandProcessor {
    static const rex::graphics::reg::DC_LUT_30_COLOR* Table(
        const rex::graphics::CommandProcessor& cp) {
        return (cp.*&GammaRampAccess::gamma_ramp_256_entry_table)();
    }
    static const rex::graphics::reg::DC_LUT_PWL_DATA* Pwl(
        const rex::graphics::CommandProcessor& cp) {
        return (cp.*&GammaRampAccess::gamma_ramp_pwl_rgb)();
    }
};

// The sync-only GPU's ramp (sync_cp.h's DisplayGamma, its mode
// DC_LUT_RW_MODE's) through ReadDisplayGamma's checks below: a ramp never
// written, or a table that isn't a gamma curve, is left out (kNone)
template <typename Warn>
GammaRamp CheckDisplayGamma(GammaRamp g, Warn&& warn) {
    if (g.mode == GammaRamp::kPwl) {
        bool written = false;
        for (const auto& step : g.pwl)
            for (uint32_t v : step) written |= v != 0;
        if (!written) {
            warn("the PWL ramp is unwritten");
            g.mode = GammaRamp::kNone;
        }
        return g;
    }
    g.mode = GammaRamp::kNone;
    if (std::all_of(std::begin(g.table), std::end(g.table), [](uint32_t v) { return v == 0; })) {
        warn("the table is unwritten");
        return g;
    }
    if (!PlausibleTable(g)) {
        warn("the table read isn't a gamma curve");
        return g;
    }
    g.mode = GammaRamp::kTable;
    return g;
}

// The display gamma ramp the presenter applies (gamma_ramp.h): the command
// processor's copy of the DC_LUT registers, which the guest writes at a swap
// after setting a ramp, and DC_LUT_RW_MODE for which of the two it wrote. Read
// through the SDK headers' inline accessors, so right only while the GPU
// plugin is built from the same headers: a table that doesn't look like a
// gamma curve is taken as misread, logged once, and left out (kNone), as is
// one never written. With renderer native, band3's sync-only GPU has the
// registers (sync_cp.h), checked the same way. Under g_state_mutex.
GammaRamp ReadDisplayGamma() {
    static bool warned = false;
    auto warn = [](const char* why) {
        if (warned) return;
        warned = true;
        REXLOG_WARN("native view: no display gamma ramp ({}); captures are drawn without one",
                    why);
    };
    // no plugin there to cast to: its GraphicsSystem isn't the runtime's
    if (sync_gpu::Band3GraphicsSystem* sync = sync_gpu::Active())
        return CheckDisplayGamma(sync->DisplayGamma(), warn);
    GammaRamp g;
    rex::system::KernelState* kernel = rex::system::kernel_state();
    rex::Runtime* runtime = kernel ? kernel->emulator() : nullptr;
    // band3's GPU is the SDK's xenos plugin, whose system is a GraphicsSystem;
    // not a dynamic_cast, as the Linux SDK keeps GraphicsSystem's typeinfo
    // hidden inside the plugin, which leaves band3 unable to link one
    auto* graphics = runtime ? static_cast<rex::graphics::GraphicsSystem*>(
                                   runtime->graphics_system())
                             : nullptr;
    const rex::graphics::CommandProcessor* cp =
        graphics ? graphics->command_processor() : nullptr;
    if (!cp) {
        warn("no command processor");
        return g;
    }
    const rex::graphics::reg::DC_LUT_30_COLOR* table = GammaRampAccess::Table(*cp);
    const rex::graphics::reg::DC_LUT_PWL_DATA* pwl = GammaRampAccess::Pwl(*cp);
    for (int i = 0; i < 256; i++) g.table[i] = table[i].value;
    for (int i = 0; i < 128 * 3; i++) g.pwl[i / 3][i % 3] = pwl[i].value;
    const uint32_t rw_mode =
        graphics->register_file()->values[rex::graphics::XE_GPU_REG_DC_LUT_RW_MODE];
    if (rw_mode & 1) {
        bool written = false;
        for (const auto& step : g.pwl)
            for (uint32_t v : step) written |= v != 0;
        if (written) g.mode = GammaRamp::kPwl;
        else warn("the PWL ramp is unwritten");
        return g;
    }
    if (std::all_of(std::begin(g.table), std::end(g.table), [](uint32_t v) { return v == 0; })) {
        warn("the table is unwritten");
        return g;
    }
    if (!PlausibleTable(g)) {
        warn("the table read isn't a gamma curve");
        return g;
    }
    g.mode = GammaRamp::kTable;
    return g;
}

// logs the ramp when it differs from the last one read (it's set at boot and
// by the game's settings, so normally once)
void LogGammaIfChanged(const GammaRamp& g) {
    static GammaRamp last;
    static bool logged = false;
    if (logged && g == last) return;
    logged = true;
    last = g;
    uint8_t lut[3][256];
    GammaLut(g, lut);
    std::string curve;
    for (int v : {0, 4, 8, 16, 32, 64, 96, 128, 192, 255})
        curve += fmt::format(" {}:{}/{}/{}", v, lut[0][v], lut[1][v], lut[2][v]);
    REXLOG_INFO("native view: display gamma ramp {} (value: shown r/g/b){}",
                g.mode == GammaRamp::kTable ? "table"
                : g.mode == GammaRamp::kPwl ? "pwl"
                                            : "none",
                curve);
}

// `from`'s vectors emptied into `to`'s, which keeps their memory: a frame's
// draws and shades come to a megabyte or two, which allocated afresh each
// frame cost the game's thread its pages' first touches and the copies as
// they grow
void Recycle(FrameCapture& from, FrameCapture& to) {
    from.draws.clear();
    from.shades.clear();
    from.passes.clear();
    from.cameras.clear();
    from.rt_filtered_keys.clear();
    to.draws.swap(from.draws);
    to.shades.swap(from.shades);
    to.passes.swap(from.passes);
    to.cameras.swap(from.cameras);
    to.rt_filtered_keys.swap(from.rt_filtered_keys);
}

// The frame's end, under g_state_mutex: the frame captured, for
// HoldIfRequested once that's let go, or null while capture is off
std::shared_ptr<const FrameCapture> FinishFrame(uint8_t* base) {
    State& s = S();
    // passes close within the frame they start in
    if (s.open.tex) DropOpenPass(s);
    const uint64_t game_frame = s.game_frame++;
    s.profile.frames++;
    // what this frame cost the hooks since the last one ended
    FrameCapture::Cost cost;
    for (int i = 0; i < CaptureProfile::kNumHooks; i++)
        cost.hook_ns[i] = s.profile.hook_ns[i] - s.cost_mark.hook_ns[i];
    cost.draws = uint32_t(s.profile.draws - s.cost_mark.draws);
    cost.new_shades = uint32_t(s.profile.new_shades - s.cost_mark.new_shades);
    cost.allocs = uint32_t(s.profile.allocs - s.cost_mark.allocs);
    cost.bones = uint32_t(s.profile.bones - s.cost_mark.bones);
    cost.geom_copy_bytes = s.profile.geom_copy_bytes - s.cost_mark.geom_copy_bytes;
    cost.tex_copy_bytes = s.profile.tex_copy_bytes - s.cost_mark.tex_copy_bytes;
    cost.tex_decode_bytes = s.profile.tex_decode_bytes - s.cost_mark.tex_decode_bytes;
    cost.game_ns = s.game_ns;
    s.cost_mark = s.profile;
    if (!g_enabled.load(std::memory_order_relaxed)) {
        FrameCapture& b = *s.building;
        if (!b.draws.empty() || !b.passes.empty() || b.post_boundary != FrameCapture::kNoPost ||
            b.passes_unbalanced) {
            s.building = std::make_shared<FrameCapture>();
            s.profile.allocs++;
        }
        s.shades.clear();
        s.samples.clear();
        s.left_out.clear();
        s.logged_no_mat = s.logged_elsewhere = 0;
        // nothing watches textures until capture or recording is on again, so
        // what's known of them would go stale (an address reused, say)
        if (!g_record_targets.load(std::memory_order_relaxed) && !s.rts.empty()) s.rts.clear();
        s.captured_last = false;
        s.last_world.reset();
        return nullptr;
    }
    s.profile.captured++;
    s.building->frame = ++s.frame;
    s.building->game_frame = game_frame;
    s.building->world_frame = game_frame;
    s.building->cost = cost;
    s.building->gamma = ReadDisplayGamma();
    LogGammaIfChanged(s.building->gamma);
    if (const Guest g{base}; const uint32_t rnd = g.U32(kDrawModeHolder)) {
        s.building->has_clear_color = 1;
        for (int i = 0; i < 4; i++)
            s.building->clear_color[i] = g.F32(rnd + kRnd_ClearColor + i * 4);
    }
    Lap(s, CaptureProfile::kStepGamma);
    CarryPasses(s, *s.building);
    std::shared_ptr<const FrameCapture> done = s.building;
    // With even/odd rendering, a frame that drew the world is kept for the
    // ones after it that don't, which present it (frame_compose.h); one that
    // began before capture was on has only part of it. The world is kept for a
    // period of frames, and dropped by a frame that doesn't say (menus).
    const bool whole = s.captured_last;
    s.captured_last = true;
    s.building->whole = whole ? 1 : 0;
    if (!ProcKnown(*done)) {
        s.last_world.reset();
    } else if (DrawsWorld(*done)) {
        s.last_world = whole ? done : nullptr;
    } else if (s.last_world && s.last_world->game_frame + MaxWorldAge() >= game_frame) {
        done = ComposeFrame(*s.last_world, *done);
        s.profile.allocs++;
    }
    Lap(s, CaptureProfile::kStepCompose);
    // the frame published before, let go of after the lock is: the worker
    // takes it for every frame, and a capture's draws and shades take a while
    // to free
    std::shared_ptr<const FrameCapture> old;
    {
        std::lock_guard lock(g_latest_mutex);
        old = std::exchange(g_latest, done);
        g_latest_published = std::chrono::steady_clock::now();
        g_capture_epoch++;
    }
    g_latest_cv.notify_all();
    auto next = std::make_shared<FrameCapture>();
    s.profile.allocs++;
    // nothing else has it (the worker drew it and let go, and it isn't the
    // world kept for composing): its emptied vectors are the next frame's,
    // memory and all (made as a FrameCapture, so not const); and room for as
    // much as this frame had
    if (old && old.use_count() == 1) Recycle(const_cast<FrameCapture&>(*old), *next);
    next->draws.reserve(done->draws.size() + done->draws.size() / 8);
    next->shades.reserve(done->shades.size() + done->shades.size() / 8);
    old.reset();
    Lap(s, CaptureProfile::kStepPublish);
    s.building = std::move(next);
    s.shades.clear();
    s.samples.clear();
    s.left_out.clear();
    s.logged_no_mat = s.logged_elsewhere = 0;
    s.cam_counted = false;
    // a venue change leaves stale entries behind; start over now and then
    if (s.geoms.size() > 50000) s.geoms.clear();
    if (s.texs.size() > 20000) s.texs.clear();
    if (s.map_texs.size() > 20000) s.map_texs.clear();
    Lap(s, CaptureProfile::kStepReset);
    return done;
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
                                                     std::chrono::milliseconds settle,
                                                     bool* fell_back) {
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
    if (fell_back) *fell_back = got && g_held.fell_back;
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
    // its deferred textures and geometry decoded now the game has gone on
    if (got && frame) DecodeDeferred(*frame);
    return got ? frame : nullptr;
}

std::shared_ptr<const FrameCapture> LatestCapture() {
    std::chrono::steady_clock::time_point published;
    return LatestCapture(published);
}

std::shared_ptr<const FrameCapture> LatestCapture(
    std::chrono::steady_clock::time_point& published, double* decode_ms) {
    std::shared_ptr<const FrameCapture> latest;
    {
        std::lock_guard lock(g_latest_mutex);
        published = g_latest_published;
        latest = g_latest;
    }
    if (!latest) return latest;
    const auto start = std::chrono::steady_clock::now();
    DecodeDeferred(*latest);
    if (decode_ms)
        *decode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               start)
                         .count();
    return latest;
}

uint64_t LatestCaptureFrame() {
    std::lock_guard lock(g_latest_mutex);
    return g_latest ? g_latest->frame : 0;
}

uint64_t CaptureEpoch() {
    std::lock_guard lock(g_latest_mutex);
    return g_capture_epoch;
}

uint64_t WaitForCapture(uint64_t epoch, std::chrono::nanoseconds timeout) {
    std::unique_lock lock(g_latest_mutex);
    g_latest_cv.wait_for(lock, timeout, [epoch] { return g_capture_epoch != epoch; });
    return g_capture_epoch;
}

void WakeCaptureWaiters() {
    {
        std::lock_guard lock(g_latest_mutex);
        g_capture_epoch++;
    }
    g_latest_cv.notify_all();
}

std::vector<std::chrono::steady_clock::time_point> GamePresentTimes(
    std::chrono::steady_clock::time_point since) {
    using Clock = std::chrono::steady_clock;
    std::vector<int64_t> ns;
    {
        std::lock_guard lock(g_present_times_mutex);
        ns = g_present_times.Since(int64_t(Nanos(since.time_since_epoch())));
    }
    std::vector<Clock::time_point> out;
    out.reserve(ns.size());
    for (int64_t t : ns) out.push_back(Clock::time_point(std::chrono::nanoseconds(t)));
    return out;
}

uint64_t GamePresentCount() {
    std::lock_guard lock(g_present_times_mutex);
    return g_present_times.Total();
}

PassRecordingStats GetPassRecordingStats() {
    PassRecordingStats out;
    out.passes = g_rec_passes.load(std::memory_order_relaxed);
    out.passes_recorded = g_rec_recorded.load(std::memory_order_relaxed);
    out.draws_recorded = g_rec_draws.load(std::memory_order_relaxed);
    out.ms = double(g_rec_ns.load(std::memory_order_relaxed)) / 1e6;
    out.on = g_record_targets.load(std::memory_order_relaxed);
    return out;
}

CaptureProfile GetCaptureProfile() {
    std::lock_guard lock(g_state_mutex);
    const State& s = S();
    CaptureProfile p = s.profile;
    p.steps_on = g_profile_steps.load(std::memory_order_relaxed);
    p.rts = s.rts.size();
    p.geoms = s.geoms.size();
    p.texs = s.texs.size();
    p.map_texs = s.map_texs.size();
    p.deferred_decodes = g_deferred_decode.decodes.load(std::memory_order_relaxed);
    p.deferred_decode_us = g_deferred_decode.ns.load(std::memory_order_relaxed) / 1000;
    p.deferred_decode_bytes = g_deferred_decode.bytes.load(std::memory_order_relaxed);
    return p;
}

CaptureProfile CaptureProfileSince(const CaptureProfile& now, const CaptureProfile& before) {
    CaptureProfile p = now;
    for (int i = 0; i < CaptureProfile::kNumHooks; i++) {
        p.hook_ns[i] -= before.hook_ns[i];
        p.hook_calls[i] -= before.hook_calls[i];
    }
    for (int i = 0; i < CaptureProfile::kNumSteps; i++) {
        p.step_ns[i] -= before.step_ns[i];
        p.step_calls[i] -= before.step_calls[i];
    }
    p.frames -= before.frames;
    p.captured -= before.captured;
    p.draws -= before.draws;
    p.new_shades -= before.new_shades;
    p.allocs -= before.allocs;
    p.geom_copy_bytes -= before.geom_copy_bytes;
    p.tex_copy_bytes -= before.tex_copy_bytes;
    p.tex_decode_bytes -= before.tex_decode_bytes;
    p.bones -= before.bones;
    p.deferred_decodes -= before.deferred_decodes;
    p.deferred_decode_us -= before.deferred_decode_us;
    p.deferred_decode_bytes -= before.deferred_decode_bytes;
    return p;
}

bool RtFallbackGuest() {
    return g_rt_fallback_guest.load(std::memory_order_relaxed) && !RendererNative();
}

}  // namespace band3::render

using namespace band3::render;

namespace {

// native_view_rt_fallback and native_view_record_targets as the render
// thread reads them, kept by their change callbacks (F4 reassigns the
// string's storage on the UI thread); registered at the first frame, once the
// cvars exist
void TrackSettings() {
    static std::once_flag tracking;
    std::call_once(tracking, [] {
        g_rt_fallback_guest.store(rex::cvar::GetFlagByName("native_view_rt_fallback") != "none");
        rex::cvar::RegisterChangeCallback("native_view_rt_fallback",
                                          [](std::string_view, std::string_view v) {
                                              g_rt_fallback_guest.store(v != "none");
                                          });
        auto profile_steps = [](std::string_view v) {
            g_profile_steps.store(v == "true" || v == "1");
        };
        profile_steps(rex::cvar::GetFlagByName("native_view_capture_profile"));
        rex::cvar::RegisterChangeCallback(
            "native_view_capture_profile",
            [profile_steps](std::string_view, std::string_view v) { profile_steps(v); });
        // Recorded while the native picture shows as well: it draws outfits
        // and the like from passes RB3 draws once (in the main menu), which
        // capture must have seen. Shown later (renderer emulated, then both),
        // it doesn't bring back the ones drawn before. And recorded from then
        // on, native picture or not: turned off, recording would forget every
        // pass it kept (FinishFrame clears s.rts) and F8 back to native would
        // show the outfits wrong, so once native it stays on for the session.
        auto record = [] {
            g_record_targets.store(g_record_targets_set.load() || g_renderer_was_native.load());
        };
        // for the session (renderer_switch.h); listened to before the picture is
        // read, so a change in between isn't missed
        AddShownPictureListener([record](bool native) {
            if (native) g_renderer_was_native.store(true);
            record();
        });
        g_record_targets_set.store(REXCVAR_GET(native_view_record_targets));
        if (ShowsNativePicture()) g_renderer_was_native.store(true);
        record();
        rex::cvar::RegisterChangeCallback("native_view_record_targets",
                                          [record](std::string_view, std::string_view v) {
                                              g_record_targets_set.store(v == "true" || v == "1");
                                              record();
                                          });
        // band3's own, else the SDK's by name: it lives in the GPU's DLL, and
        // with renderer native there's none
        auto aniso = [] {
            const std::string emulated = rex::cvar::GetFlagByName("anisotropic_override");
            std::optional<int> override_value;
            if (rex::cvar::GetFlagInfo("anisotropic_override")) {
                int value = -1;
                std::from_chars(emulated.data(), emulated.data() + emulated.size(), value);
                override_value = value;
            }
            g_aniso_override.store(
                NativeAnisotropy(REXCVAR_GET(native_anisotropic), override_value));
        };
        aniso();
        for (const char* name : {"native_anisotropic", "anisotropic_override"}) {
            rex::cvar::RegisterChangeCallback(
                name, [aniso](std::string_view, std::string_view) { aniso(); });
        }
        REXLOG_INFO("native view: anisotropy {} (native_anisotropic {}, anisotropic_override {})",
                    g_aniso_override.load(), REXCVAR_GET(native_anisotropic),
                    rex::cvar::GetFlagInfo("anisotropic_override")
                        ? rex::cvar::GetFlagByName("anisotropic_override")
                        : std::string("none: no emulated GPU"));
    });
}

}  // namespace

// The hooks below do nothing but an early-out unless capture is on or
// native_view_record_targets is (Active), so a game that never uses the
// native view pays next to nothing for them. Past it, each takes
// g_state_mutex around its own work, never around the game's function.

extern "C" REX_FUNC(RndCam__Select) {
    const uint32_t cam = ctx.r3.u32;
    // the camera at the window's shape (native_fill_window)
    band3::aspect::BeforeSelect(base, cam);
    __imp__RndCam__Select(ctx, base);
    // kept while capture is off too if texture passes are recorded then
    if (!Active()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookOther);
    State& s = S();
    if (cam != s.cam) s.cam_counted = false;
    s.cam = cam;
    s.vp_valid = false;
    s.cam_backbuffer = REX_LOAD_U32(cam + kCam_TargetTex) == 0;
    if (s.cam_backbuffer && g_enabled.load(std::memory_order_relaxed))
        RecordCamera(Guest{base}, s, cam);
}

// The draw hooks record while capturing, and inside a recorded texture pass
// while not.
//
// DxMesh::DrawShowing (band3_recomp.24.cpp) returns unless DxMesh::CanDraw;
// in the velocity buffer's draw mode it queues the mesh and draws nothing;
// else it sets the transforms and, for its material and each NextPass after
// it (none for a fur material, whose DrawFur draws its shells), calls
// RndShader::SelectConfig(mat, 18) and its geometry owner's DrawFaces
// (DxMesh::DrawFaces, through the vtable), which draws the faces with the
// device as that pass set it. Each DrawFaces is recorded as a draw of its own
// (the DrawFaces hook); a DrawShowing that drew none is recorded as before
// passes were, after it, for what Target and MeshParts count.
extern "C" REX_FUNC(DxMesh__DrawShowing) {
    const uint32_t mesh = ctx.r3.u32;
    const MeshDrawing outer = g_mesh_drawing;
    g_mesh_drawing = MeshDrawing{mesh, 0};
    __imp__DxMesh__DrawShowing(ctx, base);
    const uint32_t passes = g_mesh_drawing.passes;
    g_mesh_drawing = outer;
    if (passes || !Recording()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookMesh);
    RecordTimer timer;
    CaptureMesh(base, mesh, REX_LOAD_U32(mesh + kMesh_Mat), 0, false);
}

// DxMesh::DrawFaces (r3 the geometry owner): one material pass of the
// DxMesh::DrawShowing running, with g_selected_mat its material. Called
// outside one too, by RndTexBlender (its blend meshes, into its texture,
// after its own SelectConfig) and the velocity buffer: those are counted
// (faces_elsewhere) and not recorded.
extern "C" REX_FUNC(DxMesh__DrawFaces) {
    const uint32_t geom = ctx.r3.u32;
    __imp__DxMesh__DrawFaces(ctx, base);
    const uint32_t mesh = g_mesh_drawing.mesh;
    const uint32_t pass = mesh ? g_mesh_drawing.passes++ : 0;
    if (!Recording()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookMesh);
    RecordTimer timer;
    if (mesh) {
        CaptureMesh(base, mesh, g_selected_mat, pass);
        return;
    }
    CountFacesElsewhere(Guest{base}, geom);
}

// DxMultiMesh::DrawShowing draws in the colour pass alone, through
// DrawBatchedNewGfx, whose passes CaptureMultiMesh records: each at the next
// pass's SelectConfig (the SelectConfig hook) and the last after it. One that
// selected none (CanDraw false, another draw mode) is recorded as before
// passes were, for what Target and MeshParts count.
extern "C" REX_FUNC(DxMultiMesh__DrawShowing) {
    const uint32_t multimesh = ctx.r3.u32;
    const MultiMeshDrawing outer = g_multimesh_drawing;
    g_multimesh_drawing = MultiMeshDrawing{multimesh, 0, 0};
    __imp__DxMultiMesh__DrawShowing(ctx, base);
    const MultiMeshDrawing drawn = g_multimesh_drawing;
    g_multimesh_drawing = outer;
    if (!Recording()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookMultiMesh);
    RecordTimer timer;
    if (drawn.passes) {
        CaptureMultiMesh(base, multimesh, drawn.mat, drawn.passes - 1);
    } else {
        const uint32_t mesh = REX_LOAD_U32(multimesh + kMultiMesh_Mesh);
        CaptureMultiMesh(base, multimesh, mesh ? REX_LOAD_U32(mesh + kMesh_Mat) : 0, 0);
    }
}

// RndShader::SelectConfig(mat, ShaderType, ...): selects the material pass's
// shader and sets its constants (as the draw mode wants it: 17 for a shadow
// map's depth, 22 velocity). Inside a DxMultiMesh::DrawShowing, the pass
// before this one has drawn all its instances by now: recorded before its
// device state is replaced.
extern "C" REX_FUNC(RndShader__SelectConfig) {
    const uint32_t mat = ctx.r3.u32;
    MultiMeshDrawing& multi = g_multimesh_drawing;
    if (multi.multimesh) {
        if (multi.passes && Recording()) {
            std::lock_guard lock(g_state_mutex);
            HookTimer hook_timer(CaptureProfile::kHookMultiMesh);
            RecordTimer timer;
            CaptureMultiMesh(base, multi.multimesh, multi.mat, multi.passes - 1);
        }
        multi.passes++;
        multi.mat = mat;
    }
    g_selected_mat = mat;
    __imp__RndShader__SelectConfig(ctx, base);
}

extern "C" REX_FUNC(DxParticleSys__DrawParticles) {
    const uint32_t sys = ctx.r3.u32;
    __imp__DxParticleSys__DrawParticles(ctx, base);
    if (!Recording()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookParticles);
    RecordTimer timer;
    CaptureParticles(base, sys);
}

// DxRnd::DrawRect's shader variant, which the colour one (0x82732C70) and
// Rnd::DrawRectScreen call too: r4 the Hmx::Rect, r5 the RndMat (or 0), r6
// the ShaderType, r7 the colour, r8 and r9 a gradient's second colour (or 0)
extern "C" REX_FUNC(DxRnd__DrawRect_82733538) {
    const uint32_t rnd = ctx.r3.u32, rect = ctx.r4.u32, mat = ctx.r5.u32, color = ctx.r7.u32;
    const uint32_t color1 = ctx.r8.u32, color2 = ctx.r9.u32;
    const int32_t shader = ctx.r6.s32;
    __imp__DxRnd__DrawRect_82733538(ctx, base);
    if (!Recording()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookRect);
    RecordTimer timer;
    CaptureRect(base, rnd, rect, mat, shader, color, color1, color2);
}

// RndShader::Cache(type, options): r4 is the ShaderType, r5 the 64-bit
// option word, by value. The last one before a draw is the one it drew with
// (native_view_replay's shade summary checks the word's DIFFUSE_MAP and PRELIT
// against the material). Called for every shader, so kept to two stores.
extern "C" REX_FUNC(RndShader__Cache) {
    if (Active()) {
        g_shader_options = ctx.r5.u64;
        g_shader_type = ctx.r4.s32;
    }
    __imp__RndShader__Cache(ctx, base);
}

// DxTex::MakeDrawTarget: binds the texture r3 as the render target
extern "C" REX_FUNC(DxTex__MakeDrawTarget) {
    const uint32_t tex = ctx.r3.u32;
    __imp__DxTex__MakeDrawTarget(ctx, base);
    if (!Active()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookPass);
    BeginPass(Guest{base}, tex);
}

// DxTex::FinishDrawTarget: resolves r3, then builds its mips with DrawRect
// downsamples, which belong to its pass (a pass left open when capture goes
// off is dropped at the frame's end)
extern "C" REX_FUNC(DxTex__FinishDrawTarget) {
    const uint32_t tex = ctx.r3.u32;
    if (!Active()) {
        __imp__DxTex__FinishDrawTarget(ctx, base);
        return;
    }
    {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPass);
        State& s = S();
        if (s.open.tex == tex) s.open.in_finish = true;
    }
    __imp__DxTex__FinishDrawTarget(ctx, base);
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookPass);
    RecordTimer timer;
    EndPass(tex);
}

// DxRnd::MakeDrawTarget: back to the back buffer (or the offscreen target
// after the world), which RB3 never does inside a texture pass; one left open
// is dropped
extern "C" REX_FUNC(DxRnd__MakeDrawTarget) {
    __imp__DxRnd__MakeDrawTarget(ctx, base);
    if (!Active()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookPass);
    if (S().open.tex) DropOpenPass(S());
}

// DxCam::Select: RndCam::Select, then its target or the back buffer bound,
// its viewport set and a target cleared
extern "C" REX_FUNC(DxCam__Select) {
    const uint32_t cam = ctx.r3.u32;
    __imp__DxCam__Select(ctx, base);
    if (!Active()) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookPass);
    CameraSelected(Guest{base}, cam);
}

// RndTex::~RndTex, and DxTex::SyncBitmap, which makes the D3D texture again:
// what the texture's passes made is gone. On whatever thread lets go of it:
// at boot the main thread's loading, while the splash thread draws.
extern "C" REX_FUNC(RndTex__dt) {
    if (Active()) {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPass);
        ForgetTexture(ctx.r3.u32);
    }
    __imp__RndTex__dt(ctx, base);
}

extern "C" REX_FUNC(DxTex__SyncBitmap) {
    if (Active()) {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPass);
        ForgetTexture(ctx.r3.u32);
    }
    __imp__DxTex__SyncBitmap(ctx, base);
}

// what ProcCommands asked of the frame being drawn, as DxRnd::DoPostProcess
// found it, for LatchGpuSkip at its Present (whether the emulated GPU's
// picture is the game's again); -1 until it runs (some menus' frames don't)
namespace {
std::atomic<int32_t> g_frame_proc{-1};
}  // namespace

// DxRnd::DoPostProcess, at its start: where the frame's post-processing
// begins in its draws, what ProcCommands asked of the frame, and what its
// post-processing is set to do
extern "C" REX_FUNC(DxRnd__DoPostProcess) {
    SCOPE_profile_cpu_f("RB3 DxRnd::DoPostProcess");
    g_frame_proc.store(int32_t(REX_LOAD_U32(ctx.r3.u32 + kRnd_ProcCmds)),
                       std::memory_order_relaxed);
    if (g_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookOther);
        FrameCapture& fc = *S().building;
        if (fc.post_boundary == FrameCapture::kNoPost) {
            fc.post_boundary = uint32_t(fc.draws.size());
            fc.proc_cmds = REX_LOAD_U32(ctx.r3.u32 + kRnd_ProcCmds);
            ReadPostParams(Guest{base}, fc.post);
            // a world frame's grain is the next frame's, by the last post
            // frame's map and sampler while the proc has that map
            const State& s = S();
            if ((fc.proc_cmds & kProcWorld) && !(fc.proc_cmds & kProcPost) &&
                NoiseEnabled(fc.post) && s.noise_map && s.noise_base == fc.post.noise_map_base) {
                fc.noise_map = s.noise_map;
                fc.noise_sampler = s.noise_sampler;
            }
            // and its motion blur's object pass, the next frame's
            if ((fc.proc_cmds & kProcWorld) && !(fc.proc_cmds & kProcPost))
                EmulateVelocityObjects(Guest{base}, S(), fc, fc.velocity_objects);
        }
    }
    __imp__DxRnd__DoPostProcess(ctx, base);
}

// DxRnd::FinishPostProcess, at its start, on frames that post-process: the
// composite's constants and TheShaderMgr's flags are set, and it's about to
// draw
extern "C" REX_FUNC(DxRnd__FinishPostProcess) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookOther);
        FrameCapture& fc = *S().building;
        if (!fc.post_consts.valid) {
            const Guest g{base};
            ReadPostConsts(g, fc.post_consts);
            // RndPostProc::DoPost has moved the flicker on since DoPostProcess
            // began (UpdateColorModulation), and the composite scales by this
            if (fc.post.valid && fc.post.proc)
                fc.post.color_mod = g.F32(fc.post.proc + kPostProc_ColorMod);
            if (fc.post_consts.flags[kPostFlagNoise]) CaptureNoise(g, fc);
        }
    }
    __imp__DxRnd__FinishPostProcess(ctx, base);
}

// NgDOFProc::DoPost (r3 the proc + 0x28), after it: its last blur's taps,
// if it was on (they're stale otherwise)
extern "C" REX_FUNC(NgDOFProc__DoPost) {
    const uint32_t dof = ctx.r3.u32 - kPostProcessor;
    __imp__NgDOFProc__DoPost(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookOther);
    PostConsts& pc = S().building->post_consts;
    const Guest g{base};
    if (pc.dof_survey || !g.U8(dof + kDOF_Enabled)) return;
    pc.dof_survey = 1;
    ReadBlurTaps<8>(g, pc.dof_offsets, pc.dof_weights);
}

// Bloom_Blur (NgPostProc::DoBloom's 15-tap blurs), after the frame's first:
// level 0's taps
extern "C" REX_FUNC(Bloom_Blur) {
    __imp__Bloom_Blur(ctx, base);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookOther);
    PostConsts& pc = S().building->post_consts;
    if (pc.bloom_survey) return;
    pc.bloom_survey = 1;
    ReadBlurTaps<15>(Guest{base}, pc.bloom_offsets, pc.bloom_weights);
}

// NgSpotlightDrawer::BlurRT_824D24D0: one direction of the depth volume's
// blur (across, then down: BlurRT_824D3EC8 calls it twice). It binds the
// depth volume's EDRAM surface with D3DDevice_SetRenderTarget, draws a
// DrawRect over it sampling the texture and resolves it back, with no
// DxTex::MakeDrawTarget or FinishDrawTarget: recorded as a pass into the
// depth volume of its own (no clear), a version each, so its DrawRect lands
// there rather than in the back buffer's draws.
extern "C" REX_FUNC(NgSpotlightDrawer__BlurRT_824D24D0) {
    if (!Active()) {
        __imp__NgSpotlightDrawer__BlurRT_824D24D0(ctx, base);
        return;
    }
    uint32_t tex = 0;
    {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPass);
        const Guest g{base};
        if (const uint32_t shared = g.U32(kSpotSharedHolder))
            tex = g.U32(shared + kSpotShared_DepthVolume);
        if (tex) BeginPass(g, tex);
    }
    __imp__NgSpotlightDrawer__BlurRT_824D24D0(ctx, base);
    if (!tex) return;
    std::lock_guard lock(g_state_mutex);
    HookTimer hook_timer(CaptureProfile::kHookPass);
    RecordTimer timer;
    EndPass(tex);
}

// RndSoftParticleBuffer::DoPost (r3 its PostProcessor): clears its first
// surface through the world camera's Select (a texture pass), draws the
// particle systems RndSoftParticles queued into it in draw mode 6, which
// Target lets through for that pass, then blurs it into the second surface
// and back with two DrawRect passes. Its surfaces go in the frame's
// PostConsts for the composite.
extern "C" REX_FUNC(RndSoftParticleBuffer__DoPost) {
    if (!Active()) {
        __imp__RndSoftParticleBuffer__DoPost(ctx, base);
        return;
    }
    {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPass);
        const Guest g{base};
        State& s = S();
        const uint32_t post = ctx.r3.u32;
        const uint32_t surfaces[2] = {g.U32(post + kSoftPost_Surfaces),
                                      g.U32(post + kSoftPost_Surfaces + 4)};
        s.soft_surface = surfaces[0];
        if (g_enabled.load(std::memory_order_relaxed)) {
            PostConsts& pc = s.building->post_consts;
            pc.soft_surface[0] = surfaces[0];
            pc.soft_surface[1] = surfaces[1];
        }
    }
    __imp__RndSoftParticleBuffer__DoPost(ctx, base);
    std::lock_guard lock(g_state_mutex);
    HookTimer timer(CaptureProfile::kHookPass);
    S().soft_surface = 0;
}

extern "C" REX_FUNC(DxRnd__Present) {
    SCOPE_profile_cpu_f("RB3 DxRnd::Present");
    band3::stall_watch::PresentBegin();
    __imp__DxRnd__Present(ctx, base);
    band3::stall_watch::PresentDone();
    TrackSettings();
    uint64_t game_ns = 0;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(g_present_times_mutex);
        const int64_t since = g_present_times.Add(int64_t(Nanos(now.time_since_epoch())));
        game_ns = uint64_t(std::max<int64_t>(0, since));
    }
    std::shared_ptr<const FrameCapture> done;
    {
        std::lock_guard lock(g_state_mutex);
        HookTimer timer(CaptureProfile::kHookPresent);
        S().game_ns = game_ns;
        done = FinishFrame(base);
    }
    band3::stall_watch::FinishDone();
    // whether the emulated GPU draws the next frame, now this one is captured
    // (so it's skipped only if capture has it whole), and before it's held
    LatchGpuSkip(g_enabled.load(std::memory_order_relaxed),
                 g_record_targets.load(std::memory_order_relaxed),
                 g_frame_proc.exchange(-1, std::memory_order_relaxed));
    // the frame cap's wait for this frame's beat, under either renderer: after
    // the Present's time is taken, so the game's intervals are the cadence,
    // and before a render check's hold, after which the cap starts its beat
    // again (frame_pacing.h)
    band3::pacing::PaceFrame();
    band3::stall_watch::PaceDone();
    // held without the lock, which a texture let go of on another thread
    // meanwhile takes
    if (done) HoldIfRequested(done);
    band3::stall_watch::FrameEnd();
}
