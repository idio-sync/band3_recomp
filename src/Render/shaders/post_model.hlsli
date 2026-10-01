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
// 6EF4844D and F7E2A8FB), the bright pass F920AF5C, the 4x downsample
// 38448F55. What the composite leaves out: noise and velocity blur, which
// nothing draws natively yet.

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

// The composite's colour, from the scene, its DOF blur, the depth texture's
// value, bloom's three levels, the spotlights' depth volume and the density
// map's red, and the soft-particle buffer, all at the pixel: the DOF lerp,
// then the soft particles added (63306D35, 0F105E2D: not blurred by the DOF,
// and not in bloom's bright pass, which reads the scene), then bloom
// screen-blended (or glare added), then the spotlights' volume added (by
// c127 and the density, times c91.x), then the colour matrix, saturated once
// at the end (its alpha isn't the picture's)
float3 Composite(POST_IN(PostPass) p, float4 scene, float4 dof, float depth, float3 l0, float3 l1,
                 float3 l2, float3 volume, float density, float3 soft) {
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
    if ((f & kPostXfm) != 0u) {
        rgb = float3(dot(PostXyz(p.xfm[0]), rgb) + p.xfm[0].w,
                     dot(PostXyz(p.xfm[1]), rgb) + p.xfm[1].w,
                     dot(PostXyz(p.xfm[2]), rgb) + p.xfm[2].w);
    }
    return saturate(rgb);
}
