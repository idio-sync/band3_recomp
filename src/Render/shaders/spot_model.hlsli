// Experimental: a spotlight's cone as NgSpotlightDrawer draws it into the depth
// volume, for both of the native view's backends: mesh.hlsl compiles it as
// HLSL, spot_model.cpp as C++ for the CPU, with HLSL's vector types and
// functions from its own shim, so the two can't drift apart (as shade.hlsli
// does for lighting). Keep to what both languages share: float3/4 built from
// every component, .x .y .z .w, the operators, the shim's functions
// (spot_model.cpp), `f` on float literals and SPOT_IN for a SpotParams
// argument. The textures are sampled outside: each backend reads its own.
//
// The formula is the game's cone pixel shader's (71F5DC06C51EC39B, ShaderType
// 2 kDepthVolumeShader), checked against its microcode in xsim over 1300
// random trials with all four cases below exercised (out/research/
// spotlight_survey.md 3). Each pixel of the cone's proxy mesh adds the light
// along the view ray through it that's inside the cone and in front of the
// scene: that stretch's length in view depth, times the mean of (1 - u)^2
// over it (u the height along the axis, 0 at the apex, 1 at the beam's end),
// the cross-section's profile and the fog's attenuation, times 0.004 and the
// spot's colour. The depth volume adds the cones up (ONE ONE) in 8 bits.

float3 SpotXyz(float4 v) { return float3(v.x, v.y, v.z); }

// The scene's view depth at the pixel, from the native depth there (1/w, w
// the world camera's view depth): the game reads its depth buffer (s9) back to
// view depth by the camera's range (c89); where nothing drew it's cleared to
// the far plane.
float SpotSceneDepth(SPOT_IN(SpotParams) sp, float inv_w) {
    return inv_w > 0.0f ? 1.0f / inv_w : sp.depth_range.y;
}

// which of the shader's cases a ray falls in: whether it meets the cone's
// surface at all (both ways: the cone is infinite, and has a mirror image
// beyond its apex), and if so on which side of the apex its two crossings are
static const uint kSpotMiss = 0u;       // it doesn't: nothing
static const uint kSpotFromEye = 1u;    // the eye is inside: eye to where it leaves
static const uint kSpotThrough = 2u;    // in and out the side
static const uint kSpotToScene = 3u;    // in, never out: to the scene or the proxy
static const uint kSpotMirror = 4u;     // the mirror cone only: nothing

// The stretch of the view ray through `p` (the proxy's world position at the
// pixel) inside the cone, as distances along it from the eye, [tn, tf], both
// clamped to the nearer of the scene's view depth and the proxy: a view depth
// against a distance along the ray, as the game does. dir is the ray's unit
// direction.
struct SpotSegment {
    float3 dir;
    float tn;
    float tf;
    uint kind;
};

SpotSegment SpotRay(SPOT_IN(SpotParams) sp, float3 p, float scene_depth) {
    const float3 a = SpotXyz(sp.axis);
    const float3 o = SpotXyz(sp.eye_apex);
    const float cos2 = sp.cone.w;
    const float3 d = p - SpotXyz(sp.eye);
    const float len = sqrt(dot(d, d));
    SpotSegment s;
    s.dir = d / len;
    s.tn = 0.0f;
    s.tf = 0.0f;
    s.kind = kSpotMiss;
    // |dir.a t + o.a|^2 = cos2 |dir t + o|^2: the ray meets the cone's surface
    const float va = dot(s.dir, a);
    const float oa = dot(o, a);
    const float qa = va * va - cos2;
    const float qb = va * oa - cos2 * dot(s.dir, o);
    const float qc = oa * oa - cos2 * dot(o, o);
    const float disc = qb * qb - qa * qc;
    if (disc > 0.0f) {
        const float r = sqrt(disc);
        const float t1 = (-qb - r) / qa;
        const float t2 = (-qb + r) / qa;
        // their heights along the axis: below 0 is the mirror cone
        const float h1 = oa + t1 * va;
        const float h2 = oa + t2 * va;
        const float tmax = min(scene_depth, len);
        float tn = t2;
        float tf = t2;
        s.kind = kSpotMirror;
        if (h1 > 0.0f && h2 <= 0.0f) {
            tn = 0.0f;
            tf = t1;
            s.kind = kSpotFromEye;
        } else if (h1 > 0.0f && h2 > 0.0f) {
            tf = t1;
            s.kind = kSpotThrough;
        } else if (h1 <= 0.0f && h2 > 0.0f) {
            tf = tmax;
            s.kind = kSpotToScene;
        }
        s.tn = min(max(tn, 0.0f), tmax);
        s.tf = min(max(tf, 0.0f), tmax);
    }
    return s;
}

// The mean of (1 - u)^2 over heights un..uf, ((1-uf)^3 - (1-un)^3) / (3 (un -
// uf)) in the shader, written without its cancellation: with a = 1 - uf and b
// = 1 - un that's (a^2 + ab + b^2) / 3. 0 where un == uf, as on the 360 (0
// times the reciprocal of 0).
float SpotFalloff(float un, float uf) {
    const float a = 1.0f - uf;
    const float b = 1.0f - un;
    return un == uf ? 0.0f : (a * a + a * b + b * b) / 3.0f;
}

// Where the cross-section texture (s11) is read at `p`, (k, 0): how far
// across the beam's section p is, 0 at the middle, 1 at its planes
float SpotGoboCoord(SPOT_IN(SpotParams) sp, float3 p) {
    const float pa = dot(p, SpotXyz(sp.plane_a)) - sp.plane_a.w;
    const float pb = sp.plane_b.w - dot(p, SpotXyz(sp.plane_b));
    return abs(2.0f * pa / (pa + pb) - 1.0f);
}

// What the cone adds to the depth volume at a pixel of its proxy: p the
// proxy's world position there and w its clip w, scene_depth SpotSceneDepth's
// there, xsec_texel the cross-section texture's x at SpotGoboCoord's (what a
// shader that doesn't sample it ignores), density the density map's green at
// the pixel (which only the fog's c127.zw read; 0 while the cones draw). The
// shader writes alpha 0, which ONE ONE leaves as the clear's 1.
float3 SpotCone(SPOT_IN(SpotParams) sp, float3 p, float w, float scene_depth, float xsec_texel,
                float density) {
    const SpotSegment s = SpotRay(sp, p, scene_depth);
    const float3 eye = SpotXyz(sp.eye);
    const float3 apex = SpotXyz(sp.apex);
    const float3 a = SpotXyz(sp.axis);
    const float3 n = eye + s.dir * s.tn;
    const float3 f = eye + s.dir * s.tf;
    const float un = saturate(dot(n - apex, a) * sp.apex.w);
    const float uf = saturate(dot(f - apex, a) * sp.apex.w);
    // the stretch's length in view depth
    const float3 fwd = SpotXyz(sp.forward);
    const float depth = abs((dot(fwd, n) + sp.forward.w) - (dot(fwd, f) + sp.forward.w));
    const float xsec = lerp(1.0f, xsec_texel, sp.xsec.x);
    const float fog = saturate((w - sp.fog.x) * sp.fog.y) *
                      (sp.fog.z + sp.fog.w * density / (0.125f + density));
    const float i = 0.004f * depth * SpotFalloff(un, uf) * xsec * (1.0f - saturate(fog));
    return SpotXyz(sp.color) * i;
}
