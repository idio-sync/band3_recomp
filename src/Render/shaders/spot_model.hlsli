// A spotlight's cone into NgSpotlightDrawer's depth volume, shared by both
// backends: mesh.hlsl compiles it as HLSL, spot_model.cpp as C++ through its
// shim. Keep to what both languages share: float3/4 built from every
// component, .x .y .z .w, the operators, the shim's functions, `f` on float
// literals, SPOT_IN for a SpotParams. Textures are sampled by the callers.
//
// The game's cone PS (71F5DC06C51EC39B, ShaderType 2 kDepthVolumeShader),
// checked in xsim over 1300 random trials covering all four cases
// (spotlight_survey.md 3): the view ray's stretch inside the cone and in
// front of the scene, its view-depth length times the mean of (1 - u)^2 (u
// the height along the axis, 0 at the apex), the cross-section and the fog,
// times 0.004 and the colour. Cones add ONE ONE in 8 bits.

float3 SpotXyz(float4 v) { return float3(v.x, v.y, v.z); }

// view depth from native 1/w; the far plane where nothing drew, as the
// game's s9 read through c89 gives
float SpotSceneDepth(SPOT_IN(SpotParams) sp, float inv_w) {
    return inv_w > 0.0f ? 1.0f / inv_w : sp.depth_range.y;
}

// the shader's cases; the cone is infinite, with a mirror image past its apex
static const uint kSpotMiss = 0u;
static const uint kSpotFromEye = 1u;    // the eye is inside: eye to where it leaves
static const uint kSpotThrough = 2u;    // in and out the side
static const uint kSpotToScene = 3u;    // in, never out: to the scene or the proxy
static const uint kSpotMirror = 4u;     // the mirror cone only: nothing

// [tn, tf]: distances from the eye along the ray through proxy point p,
// clamped to min(scene depth, proxy). That compares a view depth with a ray
// distance, as the game does.
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
    // |dir.a t + o.a|^2 = cos2 |dir t + o|^2
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
        // heights along the axis; below 0 is the mirror cone
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

// The mean of (1 - u)^2 over un..uf: the shader's ((1-uf)^3 - (1-un)^3) /
// (3 (un - uf)) without its cancellation. 0 where un == uf, as on the 360.
float SpotFalloff(float un, float uf) {
    const float a = 1.0f - uf;
    const float b = 1.0f - un;
    return un == uf ? 0.0f : (a * a + a * b + b * b) / 3.0f;
}

// the cross-section texture's (s11) u: 0 mid-beam, 1 at its planes
float SpotGoboCoord(SPOT_IN(SpotParams) sp, float3 p) {
    const float pa = dot(p, SpotXyz(sp.plane_a)) - sp.plane_a.w;
    const float pb = sp.plane_b.w - dot(p, SpotXyz(sp.plane_b));
    return abs(2.0f * pa / (pa + pb) - 1.0f);
}

// p and w the proxy's world position and clip w; density the density map's
// green, read only through c127.zw (0 while the cones draw)
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
    const float3 fwd = SpotXyz(sp.forward);
    const float depth = abs((dot(fwd, n) + sp.forward.w) - (dot(fwd, f) + sp.forward.w));
    const float xsec = lerp(1.0f, xsec_texel, sp.xsec.x);
    const float fog = saturate((w - sp.fog.x) * sp.fog.y) *
                      (sp.fog.z + sp.fog.w * density / (0.125f + density));
    const float i = 0.004f * depth * SpotFalloff(un, uf) * xsec * (1.0f - saturate(fog));
    return SpotXyz(sp.color) * i;
}
