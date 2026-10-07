// RB3's post-processing maths, shared by both backends: post.hlsl compiles it
// as HLSL, post_model.cpp as C++ through its shim. Keep to what both languages
// share: float2/3/4 built from every component, .x .y .z .w, the operators,
// the shim's functions, `f` on float literals, POST_IN for a PostPass. The tap
// tables live in post_model.cpp (PostPass::taps).
//
// Checked against the game's pixel shaders (m4_shader_check.md,
// check_post.py, check_velocity.py): composite 63306D35 and variants
// (spotlights 0F105E2D, 6EF4844D, F7E2A8FB; noise 4FD49280, 0A9D6EAE,
// D1942A59; trails 30, 200030, 802000200024), bright pass F920AF5C, 4x
// downsample 38448F55, glare 2789C57F, velocity 8CDB397D and the blur along
// it (140762E9, 17175814, all with c122).

float3 PostXyz(float4 v) { return float3(v.x, v.y, v.z); }

// The camera motion blur (n5_hub_soft.md 3): RndVelocityBuffer::Draw's
// kVelocityCameraShader into a half-size RGBA8 texture. It rebuilds the world
// position from the pre-pass depth, reprojects it by the previous frame and
// stores d = prev uv - uv, clamped to +-0.02, as d * 25 + 0.5 (xy) and
// |d| * 1.7677668 (z, the mask). inv_w is 1/w, 0 where nothing drew.
float4 VelocityTexel(POST_IN(PostPass) p, float2 uv, float inv_w) {
    // w / far
    const float t = inv_w > 0.0f ? 1.0f / (p.vel_near.w * inv_w) : p.vel_depth.x;
    const float3 corner = PostXyz(p.vel_corner[0]) + PostXyz(p.vel_corner[1]) * uv.x +
                          PostXyz(p.vel_corner[2]) * uv.y;
    const float4 world = float4(p.vel_near.x + corner.x * t, p.vel_near.y + corner.y * t,
                                p.vel_near.z + corner.z * t, 1.0f);
    const float rw = 1.0f / dot(p.vel_prev[3], world);
    float dx = (0.5f - uv.x) + 0.5f * rw * dot(p.vel_prev[0], world);
    float dy = (0.5f - uv.y) - 0.5f * rw * dot(p.vel_prev[1], world);
    // max then min, as the shader, so NaN comes out -0.02 on both backends
    dx = min(max(dx, -0.02f), 0.02f);
    dy = min(max(dy, -0.02f), 0.02f);
    return float4(dx * 25.0f + 0.5f, dy * 25.0f + 0.5f, sqrt(dx * dx + dy * dy) * 1.7677668f,
                  0.0f);
}

// The composite's blur where the mask >= kVelocityMask: the centre plus taps
// k = -5..4 (bilinear, clamped: s6), a Gaussian of sigma 2 steps, asymmetric
// as the shader has it, normalised by kVelocityNorm. The blurred scene
// replaces the scene in the composite; the DOF and bloom levels stay unblurred.
static const float kVelocityMask = 0.003f;
static const float kVelocityCentre = 0.19947115f;
static const float kVelocityNorm = 1.0054859f;
static const float kVelocityWeights[10] = {0.0087641505f, 0.026995484f, 0.0647588f,
                                           0.12098536f,   0.17603266f,  0.17603266f,
                                           0.12098536f,   0.0647588f,   0.026995484f,
                                           0.0087641505f};

bool VelocityBlurs(POST_IN(PostPass) p, float4 velocity) {
    return (p.flags.x & kPostVelocity) != 0u && velocity.z >= kVelocityMask;
}

// 0.115 * d * c122.x in uv
float2 VelocityStep(POST_IN(PostPass) p, float4 velocity) {
    return float2((velocity.x * 0.0046f - 0.0023f) * p.vel_depth.y,
                  (velocity.y * 0.0046f - 0.0023f) * p.vel_depth.y);
}

float2 VelocityTap(float2 uv, float2 step, int k) {
    return float2(uv.x + float(k) * step.x, uv.y + float(k) * step.y);
}

float VelocityWeight(int k) { return kVelocityWeights[k + 5]; }

// The object pass (RndVelocityBuffer::DrawMesh; VelocityObject): the motion
// blur's meshes drawn over the camera pass with their own motion, depth
// tested among themselves. VS 21A0C657 skinned, F922317D not; PS 39DE58D4.
// Skinning takes three weights and 1 - their sum.
float4 VelocityObjectWeights(float4 w) { return float4(w.x, w.y, w.z, 1.0f - (w.x + w.y + w.z)); }

// VS c0..c3, or with `last` c4..c7
float4 VelocityObjectClip(POST_IN(VelocityObjectPass) p, float3 world, bool last) {
    const int r = last ? 4 : 0;
    const float4 at = float4(world.x, world.y, world.z, 1.0f);
    return float4(dot(p.view_proj[r], at), dot(p.view_proj[r + 1], at),
                  dot(p.view_proj[r + 2], at), dot(p.view_proj[r + 3], at));
}

float2 VelocityObjectUv(float4 clip) {
    const float r = 1.0f / clip.w;
    return float2(0.5f + 0.5f * r * clip.x, 0.5f - 0.5f * r * clip.y);
}

// 1 - near / w, smaller nearer, clipping at the near plane as the game's
// does (both backends give clip z = w - near)
float VelocityObjectDepth(POST_IN(VelocityObjectPass) p, float w) {
    return 1.0f - p.depth_range.x / w;
}

// d encoded as VelocityTexel's; alpha 1 where the mesh's w <= the scene's
// view depth + 1, else 0, so the SrcAlpha blend keeps the camera's texel.
// inv_w is the pre-pass depth (s9, point; 1/w, 0 where nothing drew, which c8
// makes the far plane).
float4 VelocityObjectTexel(POST_IN(VelocityObjectPass) p, float4 cur, float4 prev, float inv_w) {
    const float near_plane = p.depth_range.x, far_plane = p.depth_range.y;
    float scene = 0.0f;
    if (inv_w > 0.0f) {
        scene = 1.0f / inv_w;
    } else {
        const float z = p.depth_range.z - p.depth_range.w;
        scene = near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
    }
    const float2 at = VelocityObjectUv(cur);
    const float2 was = VelocityObjectUv(prev);
    const float dx = min(max(was.x - at.x, -0.02f), 0.02f);
    const float dy = min(max(was.y - at.y, -0.02f), 0.02f);
    return float4(dx * 25.0f + 0.5f, dy * 25.0f + 0.5f, sqrt(dx * dx + dy * dy) * 1.7677668f,
                  cur.w > scene + 1.0f ? 0.0f : 1.0f);
}

// The bright pass and 4x downsample: four bilinear taps at uv +-2 c15, the
// 4x4 source texels under a pixel
float2 QuadTap(float2 uv, float4 half_pixel, int i) {
    const float sx = (i == 1 || i == 3) ? 1.0f : -1.0f;
    const float sy = i >= 2 ? 1.0f : -1.0f;
    return float2(uv.x + sx * 2.0f * half_pixel.x, uv.y + sy * 2.0f * half_pixel.y);
}

// the bright pass weights each tap by its alpha (PSEUDO_HDR's bloom weight),
// with no threshold
float4 Quad(float4 a, float4 b, float4 c, float4 d, bool bright) {
    if (bright) return (a * a.w + b * b.w + c * c.w + d * d.w) * 0.25f;
    return (a + b + c + d) * 0.25f;
}

// The glare pass (kBloomGlareShader, PS 2789C57F87CFFD5D), drawn by
// NgPostProc::DoBloom over bloom's level 0: taps from uv toward the uv
// mirrored through the centre, each GlareTerm'd, summed into GlareOut.
// The shader's loop constant i31.
static const int kGlareTaps = 10;

float2 GlareStep(float2 uv) {
    return float2((1.0f - 2.0f * uv.x) * 0.1f, (1.0f - 2.0f * uv.y) * 0.1f);
}

// (1 - 4 r^2)^2, r the uv distance from the centre; 0 from r = 0.5
float GlareWeight(float2 at) {
    const float dx = at.x - 0.5f;
    const float dy = at.y - 0.5f;
    const float w = 1.0f - 4.0f * min(dx * dx + dy * dy, 0.25f);
    return w * w;
}

// 1 / (1 - weight * texel); the max keeps a white texel at the centre finite,
// where the output saturates anyway
float3 GlareTerm(float3 texel, float weight) {
    return float3(1.0f / max(1.0f - weight * texel.x, 1e-6f),
                  1.0f / max(1.0f - weight * texel.y, 1e-6f),
                  1.0f / max(1.0f - weight * texel.z, 1e-6f));
}

// saturated as the game's 8-bit target would
float3 GlareOut(float3 sum) {
    return saturate(float3(2.0f - 20.0f / sum.x, 2.0f - 20.0f / sum.y, 2.0f - 20.0f / sum.z));
}

// The game's depth texture (s9) value, 1 - D3D z, for native inv_w (1/w, 0
// where nothing drew), as NgDOFProc::Set's c24 assumes
float GameDepth(POST_IN(PostPass) p, float inv_w) {
    const float near_plane = p.camera.x;
    const float far_plane = p.camera.y;
    const float z = (far_plane - far_plane * near_plane * inv_w) / (far_plane - near_plane) *
                        (p.camera.w - p.camera.z) +
                    p.camera.z;
    return 1.0f - z;
}

// 0 in focus, 1 fully blurred; the outer abs is the shader's (matters only
// for c24.w < 0, which NgDOFProc never sets)
float DofAmount(float4 c24, float depth) {
    const float t = (1.0f - depth) * c24.x + c24.y;
    return saturate(abs(min(max(abs(t), c24.z), c24.w)));
}

// The noise (NgPostProc::CheckNoise; n1_post_noise.md): tap 0 at
// (uv + c112.xy) * c113.xy, tap 1 at (uv + c112.zw) * c113.xy * c113.z,
// linear, wrapping. c112 is random each frame.
float2 NoiseUv(POST_IN(PostPass) p, float2 uv, int tap) {
    const float sx = tap == 0 ? p.noise.x : p.noise.x * p.noise.z;
    const float sy = tap == 0 ? p.noise.y : p.noise.y * p.noise.z;
    const float ox = tap == 0 ? p.noise_seeds.x : p.noise_seeds.z;
    const float oy = tap == 0 ? p.noise_seeds.y : p.noise_seeds.w;
    return float2((uv.x + ox) * sx, (uv.y + oy) * sy);
}

// a tap's uv derivatives per target pixel, as the shader's LOD sees them, so
// both backends pick the same mips
float2 NoiseDx(POST_IN(PostPass) p, int tap) {
    return float2((tap == 0 ? p.noise.x : p.noise.x * p.noise.z) * p.target.z, 0.0f);
}
float2 NoiseDy(POST_IN(PostPass) p, int tap) {
    return float2(0.0f, (tap == 0 ? p.noise.y : p.noise.y * p.noise.z) * p.target.w);
}

// An overlay of the taps' geometric mean, branching on luminance L rather
// than per channel, moved toward by c113.w (times 6.75 L (1 - L)^2 with the
// midtone weight). Unsaturated. Without the midtone weight is a guess: no
// such shader was dumped and no proc turns it off.
float3 NoiseTerm(POST_IN(PostPass) p, float3 rgb, float3 tap0, float3 tap1) {
    const float3 n = float3(sqrt(abs(tap0.x * tap1.x)), sqrt(abs(tap0.y * tap1.y)),
                            sqrt(abs(tap0.z * tap1.z)));
    const float l = dot(rgb, float3(0.30f, 0.59f, 0.11f));
    float3 over;
    if (l <= 0.5f) {
        over = float3(2.0f * n.x * rgb.x, 2.0f * n.y * rgb.y, 2.0f * n.z * rgb.z);
    } else {
        over = float3(1.0f - 2.0f * (1.0f - n.x) * (1.0f - rgb.x),
                      1.0f - 2.0f * (1.0f - n.y) * (1.0f - rgb.y),
                      1.0f - 2.0f * (1.0f - n.z) * (1.0f - rgb.z));
    }
    float w = p.noise.w;
    if ((p.flags.x & kPostNoiseMidtone) != 0u) w = (1.0f - l) * (1.0f - l) * l * 6.75f * p.noise.w;
    return float3(rgb.x + (over.x - rgb.x) * w, rgb.y + (over.y - rgb.y) * w,
                  rgb.z + (over.z - rgb.z) * w);
}

// DOF lerp, soft particles (63306D35, 0F105E2D: after the DOF, not in
// bloom), bloom screen-blended or glare added, spotlights, noise, colour
// matrix. Unsaturated, as Trails compares it.
float3 CompositeColor(POST_IN(PostPass) p, float4 scene, float4 dof, float depth, float3 l0,
                      float3 l1, float3 l2, float3 volume, float density, float3 soft,
                      float3 noise0, float3 noise1) {
    const uint f = p.flags.x;
    float3 rgb = PostXyz(scene);
    if ((f & kPostDof) != 0u) rgb = lerp(rgb, PostXyz(dof), DofAmount(p.c24, depth));
    if ((f & kPostSoft) != 0u) rgb = rgb + soft;
    const float3 c6 = PostXyz(p.c6);
    if ((f & kPostBloom) != 0u) {
        const float3 b = (l0 + l1 + l2) * c6;
        rgb = float3(1.0f - (1.0f - rgb.x) * (1.0f - b.x), 1.0f - (1.0f - rgb.y) * (1.0f - b.y),
                     1.0f - (1.0f - rgb.z) * (1.0f - b.z));
    }
    if ((f & kPostGlare) != 0u) rgb = rgb + l0 * c6 * 0.5f;
    if ((f & kPostSpot) != 0u) rgb = rgb + volume * (p.spot.x + p.spot.y * density) * p.spot.z;
    if ((f & kPostNoise) != 0u) rgb = NoiseTerm(p, rgb, noise0, noise1);
    if ((f & kPostXfm) != 0u) {
        rgb = float3(dot(PostXyz(p.xfm[0]), rgb) + p.xfm[0].w,
                     dot(PostXyz(p.xfm[1]), rgb) + p.xfm[1].w,
                     dot(PostXyz(p.xfm[2]), rgb) + p.xfm[2].w);
    }
    return rgb;
}

// without the trails
float3 Composite(POST_IN(PostPass) p, float4 scene, float4 dof, float depth, float3 l0, float3 l1,
                 float3 l2, float3 volume, float density, float3 soft, float3 noise0,
                 float3 noise1) {
    return saturate(CompositeColor(p, scene, dof, depth, l0, l1, l2, volume, density, soft, noise0,
                                   noise1));
}

// kept in the post buffer for the next frame's trails (63306D35, 0F105E2D,
// 6EF4844D)
float CompositeAlpha(POST_IN(PostPass) p, float4 scene, float4 dof, float depth) {
    float a = scene.w;
    if ((p.flags.x & kPostDof) != 0u) a = a + (dof.w - a) * DofAmount(p.c24, depth);
    return saturate(a);
}

// The trails (NgPostProc::CheckBlendPrevious, BLENDPREVIOUS), last: prev is
// s14, SavePostBuffer's copy of the last composite. Faded prev replaces the
// pixel where its score beats the pixel's unsaturated mean; alpha out is 1
// for the trail, 0 for the colour. Comparing a saturated colour, or dropping
// the alpha gate, fails the xsim checks (variants 30, 200030, 802000200024).
float4 Trails(POST_IN(PostPass) p, float3 rgb, float4 prev) {
    const float3 d = saturate(float3(prev.x - p.trails.y, prev.y - p.trails.y,
                                     prev.z - p.trails.y));
    const float m = d.x * p.trails.z + d.y * p.trails.z + d.z * p.trails.z;
    const float score = m > p.trails.x ? m : prev.w * m;
    const float mine = rgb.x * p.trails.z + rgb.y * p.trails.z + rgb.z * p.trails.z;
    if (score > mine) return float4(d.x, d.y, d.z, 1.0f);
    const float3 c = saturate(rgb);
    return float4(c.x, c.y, c.z, 0.0f);
}
