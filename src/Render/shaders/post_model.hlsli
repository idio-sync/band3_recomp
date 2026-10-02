// Experimental: RB3's post-processing maths, for both of the native view's
// backends. post.hlsl compiles it as HLSL; post_model.cpp compiles it as C++
// for the CPU, with HLSL's vector types and functions from its own shim, so
// the two can't drift apart (as shade.hlsli does for lighting). Keep to what
// both languages share: float2/3/4 built from every component, .x .y .z .w,
// the operators, the shim's functions (post_model.cpp), `f` on float
// literals and POST_IN for a PostPass argument. The tap tables are C++'s
// (post_model.cpp), handed to the GPU in PostPass::taps.
//
// The formulas are the game's pixel shaders', run in an interpreter against
// models (out/research/m4_shader_check.md, m4_shaders/check_post.py): the
// composite 63306D35 and its variants (the spotlights' term from 0F105E2D,
// 6EF4844D and F7E2A8FB, the noise from 4FD49280, 0A9D6EAE and D1942A59,
// the trails from 30, 200030 and 802000200024 of the game's shader cache),
// the bright pass F920AF5C, the 4x downsample 38448F55, the glare pass
// 2789C57F. What the composite leaves out: velocity blur, which nothing
// draws natively yet.

float3 PostXyz(float4 v) { return float3(v.x, v.y, v.z); }

// The bright pass and the 4x downsample read four bilinear taps, at uv plus
// and minus twice c15 (half a source texel) each way: on a 4x smaller target
// that's the 4x4 source texels under a pixel. Tap i is (-,-), (+,-), (-,+),
// (+,+) for i 0..3.
float2 QuadTap(float2 uv, float4 half_pixel, int i) {
    const float sx = (i == 1 || i == 3) ? 1.0f : -1.0f;
    const float sy = i >= 2 ? 1.0f : -1.0f;
    return float2(uv.x + sx * 2.0f * half_pixel.x, uv.y + sy * 2.0f * half_pixel.y);
}

// their mean; the bright pass weights each tap by its alpha (the bloom
// weight PSEUDO_HDR shaders write), alpha too, and has no threshold
float4 Quad(float4 a, float4 b, float4 c, float4 d, bool bright) {
    if (bright) return (a * a.w + b * b.w + c * c.w + d * d.w) * 0.25f;
    return (a + b + c + d) * 0.25f;
}

// The glare pass (kBloomGlareShader, bloom_glare's PS 2789C57F87CFFD5D),
// which NgPostProc::DoBloom draws over bloom's blurred level 0 when glare is
// on, and which the composite then adds: kGlareTaps taps of the level from
// the pixel's uv toward the picture's centre, GlareStep apart, so the last is
// 0.9 of the way to the uv mirrored through the centre (a ghost of the
// bright parts across it). Each tap is weighted by GlareWeight at its uv and
// taken as 1 / (1 - weight * texel); the output is GlareOut of their sum,
// rgb, alpha 1. For a dim level that's about 2 * the weighted taps' mean.
// Its count is the loop constant i31 (10) in the shader's definitions.
static const int kGlareTaps = 10;

// how far apart its taps are, from the pixel's uv: 0.1 * (1 - 2 uv)
float2 GlareStep(float2 uv) {
    return float2((1.0f - 2.0f * uv.x) * 0.1f, (1.0f - 2.0f * uv.y) * 0.1f);
}

// a tap's weight at its uv: (1 - 4 r^2)^2, r its distance from the centre in
// uv, 0 from r = 0.5 out
float GlareWeight(float2 at) {
    const float dx = at.x - 0.5f;
    const float dy = at.y - 0.5f;
    const float w = 1.0f - 4.0f * min(dx * dx + dy * dy, 0.25f);
    return w * w;
}

// a tap's term. The shader takes the reciprocal as it is, which is infinite
// only at the centre of a white texel (weight 1): there the output saturates
// either way, and the max keeps it finite
float3 GlareTerm(float3 texel, float weight) {
    return float3(1.0f / max(1.0f - weight * texel.x, 1e-6f),
                  1.0f / max(1.0f - weight * texel.y, 1e-6f),
                  1.0f / max(1.0f - weight * texel.z, 1e-6f));
}

// the output from the taps' terms summed: 2 - 20 / sum, 0 for a black level,
// saturated as the 8-bit target keeps it (the shader leaves that to it)
float3 GlareOut(float3 sum) {
    return saturate(float3(2.0f - 20.0f / sum.x, 2.0f - 20.0f / sum.y, 2.0f - 20.0f / sum.z));
}

// What the game's depth texture (s9) holds where the native depth is inv_w,
// 1/w with w the world camera's view depth (0 where nothing drew, w
// infinite): 1 - z, z the D3D depth the camera's projection gives, its near
// and far planes mapped into its z range, as NgDOFProc::Set works out c24's
// scale and bias for the focal plane.
float GameDepth(POST_IN(PostPass) p, float inv_w) {
    const float near_plane = p.camera.x;
    const float far_plane = p.camera.y;
    const float z = (far_plane - far_plane * near_plane * inv_w) / (far_plane - near_plane) *
                        (p.camera.w - p.camera.z) +
                    p.camera.z;
    return 1.0f - z;
}

// how much of the blurred scene the composite takes at a pixel: 0 in focus,
// 1 at the blur's full depth either side (the second abs is the shader's; it
// only matters for a c24.w below 0, which NgDOFProc never sets)
float DofAmount(float4 c24, float depth) {
    const float t = (1.0f - depth) * c24.x + c24.y;
    return saturate(abs(min(max(abs(t), c24.z), c24.w)));
}

// The noise (NgPostProc::CheckNoise's; out/research/n1_post_noise.md): the
// composite reads the noise map twice, tap 0 at (uv + c112.xy) * c113.xy and
// tap 1 at (uv + c112.zw) * c113.xy * c113.z, linear and wrapping. c112 is
// four random numbers each frame (two fixed ones when stationary, and c113.z
// 1), so the grain moves every frame.
float2 NoiseUv(POST_IN(PostPass) p, float2 uv, int tap) {
    const float sx = tap == 0 ? p.noise.x : p.noise.x * p.noise.z;
    const float sy = tap == 0 ? p.noise.y : p.noise.y * p.noise.z;
    const float ox = tap == 0 ? p.noise_seeds.x : p.noise_seeds.z;
    const float oy = tap == 0 ? p.noise_seeds.y : p.noise_seeds.w;
    return float2((uv.x + ox) * sx, (uv.y + oy) * sy);
}

// a tap's uv derivatives one pixel across (dx) and down (dy) the composite's
// target: its scale over the target's size, as the shader's computed LOD
// takes them from the screen-filling quad, so both backends read the same
// mip levels (sample_model.hlsli's SampleTexture takes them)
float2 NoiseDx(POST_IN(PostPass) p, int tap) {
    return float2((tap == 0 ? p.noise.x : p.noise.x * p.noise.z) * p.target.z, 0.0f);
}
float2 NoiseDy(POST_IN(PostPass) p, int tap) {
    return float2(0.0f, (tap == 0 ? p.noise.y : p.noise.y * p.noise.z) * p.target.w);
}

// The grain over the colour so far, from the two taps' rgb: per channel n =
// sqrt(|tap0 * tap1|), the taps' geometric mean; overlaid on the colour by
// its luminance L (the overlay's branch by L, not per channel), 2 n rgb at L
// 0.5 or below, else 1 - 2 (1 - n)(1 - rgb); and the colour moved toward
// that by c113.w, times 6.75 L (1 - L)^2 with the midtone weight (1 at L =
// 1/3, 0 at black and white). Nothing saturates it: the composite does once,
// at the end. Without the midtone weight the move is c113.w alone, a guess
// (no shader without it was dumped, and no proc in the game has it off).
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

// The composite's colour, from the scene, its DOF blur, the depth texture's
// value, bloom's three levels, the spotlights' depth volume and the density
// map's red, the soft-particle buffer and the noise map's two taps, all at
// the pixel: the DOF lerp, then the soft particles added (63306D35,
// 0F105E2D: not blurred by the DOF, and not in bloom's bright pass, which
// reads the scene), then bloom screen-blended (or glare added), then the
// spotlights' volume added (by c127 and the density, times c91.x), then the
// noise (NoiseTerm), then the colour matrix; unsaturated, as the trails
// (Trails) compare it, which Composite saturates once (its alpha isn't the
// picture's: CompositeAlpha)
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

// the composite's colour, saturated, without the trails
float3 Composite(POST_IN(PostPass) p, float4 scene, float4 dof, float depth, float3 l0, float3 l1,
                 float3 l2, float3 volume, float density, float3 soft, float3 noise0,
                 float3 noise1) {
    return saturate(CompositeColor(p, scene, dof, depth, l0, l1, l2, volume, density, soft, noise0,
                                   noise1));
}

// The composite's alpha, which the post buffer keeps for the next frame's
// trails: the scene's (the bloom weight), lerped toward the DOF level's by
// the DOF amount, saturated (63306D35, 0F105E2D, 6EF4844D; the scene's alone
// without DOF)
float CompositeAlpha(POST_IN(PostPass) p, float4 scene, float4 dof, float depth) {
    float a = scene.w;
    if ((p.flags.x & kPostDof) != 0u) a = a + (dof.w - a) * DofAmount(p.c24, depth);
    return saturate(a);
}

// The trails (blend previous: NgPostProc::CheckBlendPrevious sets c125 =
// (mTrailThreshold, dt / mTrailDuration, 1/3, 0) and TheShaderMgr + 0x2F,
// the composite's BLENDPREVIOUS option), its last step, after the colour
// matrix: the previous post frame (s14, the post buffer SavePostBuffer
// resolved from the last composite, its colour and alpha) faded by c125.y,
// d = sat(prev - c125.y); its mean m (c125.z) scores m where m is over the
// threshold c125.x, else m times the previous alpha (1 where it was kept
// there, else the composite's alpha); the faded colour replaces the pixel's
// where its score beats the pixel's own mean (unsaturated), and the alpha
// out says which was kept: 1 the trail, 0 the colour. The colour comes out
// saturated. Checked in xsim against the game's variants 30, 200030 and
// 802000200024 (glare, noise, colour matrix: the music-video venues'); a
// saturated colour compared, or no alpha gate, fails them. A fade of a
// sixth or more each frame under a threshold of 0.9999 (most music-video
// procs) can never start a trail, so there it's the colour alone after the
// first few frames; video_trails' (0, dt / 0.4) leaves real trails.
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
