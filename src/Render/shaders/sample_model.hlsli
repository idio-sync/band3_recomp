// Experimental: how the native view's backends read a material's texture
// through the sampler its fetch constant describes (scene_capture.h's
// TexSampler, packed by sample_model.h's PackSampler), for both of them:
// mesh.hlsl compiles it as HLSL, soft_raster.cpp as C++ for the CPU, with
// HLSL's types and functions from its own shim, so the two can't drift apart
// (as shade.hlsli does for lighting). Keep to what both languages share:
// float2/4 built from every component, .x .y .z .w, the shim's functions
// (soft_raster.cpp), `f` on float literals, and the includer's macros:
// SAMPLE_TEX, the texture's parameters (a texture array and the layer on the
// GPU, its levels on the CPU), SAMPLE_ARGS, those passed on,
// SAMPLE_LOAD(level, x, y), its texel there (in range: the addressing below
// has wrapped or clamped it), and SAMPLE_LOOP, HLSL's [loop] (nothing in C++).
//
// A sample is the D3D one: the footprint of a pixel in the texture, from its
// uv's derivatives across the screen (the GPU's ddx_fine/ddy_fine, which the
// CPU works out the same way from the triangle's attribute plane), in level
// 0's texels; LOD log2 of its longer side (divided by the probes, with
// anisotropy) plus the sampler's bias; magnification at LOD 0 or less, with
// its own filter; the level, or the two around it, from the LOD clamped to
// the sampler's mip range and the levels the texture has; each point or
// bilinear (texel centres at .5); with anisotropy up to its ratio of such
// samples spread along the footprint's long side, averaged (the Vulkan
// spec's approximation). Addressing per axis, as the sampler's clamp modes
// say: repeat, mirror, clamp to the edge or to the border, or mirror once
// then clamp (halfway is drawn as the edge). The Xenos' own filter weights
// and LOD precision aren't modelled.

// TexSampler as a uint4: x the modes, y the LOD bias's bits, z the mip range
// and the texture's last level, w the anisotropy
static const uint kSampleFiltered = 1u << 11;

// texel i of n along an axis, by clamp mode `mode`; -1 is the border. i is
// within a period of the texture either side, where SampleWrap's coordinate
// puts it: no modulo, which costs the GPU dearly
int SampleAddress(int i, int n, uint mode) {
    if (mode == 0u) return i < 0 ? i + n : i >= n ? i - n : i;
    if (mode == 1u) {
        const int p = 2 * n;
        const int m = i < 0 ? i + p : i >= p ? i - p : i;
        return m < n ? m : p - 1 - m;
    }
    // 3, 5 and 7 mirror once about 0
    int j = i;
    if ((mode & 1u) != 0u && j < 0) j = -1 - j;
    if (mode >= 6u) return j < 0 || j >= n ? -1 : j;
    return j < 0 ? 0 : j >= n ? n - 1 : j;
}

// a coordinate brought near the texture first, so it stays exact in float
// and in int: the wrapping modes by their period, the others to a texture
// either side, past which every mode reads the same
float SampleWrap(float u, uint mode) {
    if (mode == 0u) return u - floor(u);
    if (mode == 1u) return u - 2.0f * floor(u * 0.5f);
    return u < -2.0f ? -2.0f : u > 2.0f ? 2.0f : u;
}

// texel x, y of a level (addressed already: -1 the border), or the border
float4 SampleTap(SAMPLE_TEX, uint level, int x, int y, float4 border) {
    return x < 0 || y < 0 ? border : SAMPLE_LOAD(level, x, y);
}

float4 SampleLerp(float4 a, float4 b, float t) {
    return float4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// one level at u, v (wrapped), point or bilinear
float4 SampleLevelAt(SAMPLE_TEX, uint2 size, uint level, bool bilinear, uint4 s, float u, float v) {
    const int w = int(max(size.x >> level, 1u));
    const int h = int(max(size.y >> level, 1u));
    const uint cx = s.x & 7u;
    const uint cy = (s.x >> 3) & 7u;
    const float b = ((s.x >> 10) & 1u) != 0u ? 1.0f : 0.0f;
    const float4 border = float4(b, b, b, b);
    const float shift = bilinear ? 0.5f : 0.0f;
    const float x = u * float(w) - shift;
    const float y = v * float(h) - shift;
    const float fx = floor(x);
    const float fy = floor(y);
    const int x0 = SampleAddress(int(fx), w, cx);
    const int y0 = SampleAddress(int(fy), h, cy);
    if (!bilinear) return SampleTap(SAMPLE_ARGS, level, x0, y0, border);
    const int x1 = SampleAddress(int(fx) + 1, w, cx);
    const int y1 = SampleAddress(int(fy) + 1, h, cy);
    const float4 top = SampleLerp(SampleTap(SAMPLE_ARGS, level, x0, y0, border),
                                  SampleTap(SAMPLE_ARGS, level, x1, y0, border), x - fx);
    const float4 bottom = SampleLerp(SampleTap(SAMPLE_ARGS, level, x0, y1, border),
                                     SampleTap(SAMPLE_ARGS, level, x1, y1, border), x - fx);
    return SampleLerp(top, bottom, y - fy);
}

// What a sample reads before its taps: the levels and their blend, the
// filter, and the probes along the long side (step from one to the next)
struct SamplePlan {
    uint level0;
    uint level1;
    float blend;  // of level1
    bool bilinear;
    uint probes;
    float2 step;
};

SamplePlan PlanSample(uint2 size, uint4 s, float2 dx, float2 dy) {
    const float ax = dx.x * float(size.x);
    const float ay = dx.y * float(size.y);
    const float bx = dy.x * float(size.x);
    const float by = dy.y * float(size.y);
    const float px = sqrt(ax * ax + ay * ay);
    const float py = sqrt(bx * bx + by * by);
    const float pmax = max(px, py);
    const float pmin = min(px, py);
    const uint ratio = max(s.w, 1u);
    float n = 1.0f;
    if (ratio > 1u && pmax > 0.0f)
        n = pmin > 0.0f ? min(ceil(pmax / pmin), float(ratio)) : float(ratio);
    const float lod = (pmax > 0.0f ? log2(pmax / n) : -64.0f) + asfloat(s.y);
    SamplePlan p;
    p.bilinear = ((s.x >> (lod <= 0.0f ? 6u : 7u)) & 1u) != 0u;
    const uint last = (s.z >> 8) & 15u;
    const uint hi = min((s.z >> 4) & 15u, last);
    const uint lo = min(s.z & 15u, hi);
    const float d = lod < float(lo) ? float(lo) : lod > float(hi) ? float(hi) : lod;
    const uint mip = (s.x >> 8) & 3u;
    p.blend = 0.0f;
    if (mip == 2u) {
        p.level0 = lo;
    } else if (mip == 0u) {
        p.level0 = uint(floor(d + 0.5f));
    } else {
        p.level0 = uint(floor(d));
        p.blend = d - floor(d);
    }
    p.level1 = min(p.level0 + 1u, hi);
    p.probes = uint(n);
    const float2 major = px >= py ? dx : dy;
    p.step = float2(major.x / n, major.y / n);
    return p;
}

// The texture at uv, whose derivatives across the screen are dx and dy, by
// sampler s; size is level 0's
float4 SampleTexture(SAMPLE_TEX, uint2 size, uint4 s, float2 uv, float2 dx, float2 dy) {
    const SamplePlan p = PlanSample(size, s, dx, dy);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float first = -0.5f * float(p.probes - 1u);
    // each probe's one or two levels, as one loop: one copy of the level's
    // taps in the GPU's code, not four (the shader reads five textures so)
    const uint two = p.blend > 0.0f ? 1u : 0u;
    const float k = 1.0f / float(p.probes);
    SAMPLE_LOOP for (uint tap = 0u; tap < (p.probes << two); tap++) {
        const float along = first + float(tap >> two);
        const bool upper = (tap & two) != 0u;
        const float u = SampleWrap(uv.x + p.step.x * along, s.x & 7u);
        const float v = SampleWrap(uv.y + p.step.y * along, (s.x >> 3) & 7u);
        const float4 c =
            SampleLevelAt(SAMPLE_ARGS, size, upper ? p.level1 : p.level0, p.bilinear, s, u, v);
        const float w = (two == 0u ? 1.0f : upper ? p.blend : 1.0f - p.blend) * k;
        sum = float4(sum.x + c.x * w, sum.y + c.y * w, sum.z + c.z * w, sum.w + c.w * w);
    }
    return sum;
}
