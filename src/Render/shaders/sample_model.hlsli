// Texture sampling by a fetch constant's sampler (TexSampler, packed by
// PackSampler), shared by both backends: mesh.hlsl compiles it as HLSL,
// soft_raster.cpp as C++ through its shim. Keep to what both languages share:
// float2/4 built from every component, .x .y .z .w, the shim's functions, `f`
// on float literals, and the includer's macros: SAMPLE_TEX (the texture's
// parameters), SAMPLE_ARGS (them passed on), SAMPLE_LOAD(level, x, y) (an
// in-range texel) and SAMPLE_LOOP (HLSL's [loop]).
//
// D3D-style: LOD from the uv derivatives' footprint in level 0 texels, plus
// bias; anisotropy averages up to its ratio of probes along the long side
// (Vulkan's approximation). Mirror-once-then-clamp's halfway mode draws as
// the edge. The Xenos' own filter weights and LOD precision aren't modelled.

// x the modes, y the LOD bias's bits, z the mip range and the texture's last
// level, w the anisotropy
static const uint kSampleFiltered = 1u << 11;

// -1 is the border. i is within a period either side (SampleWrap), so no
// modulo, which costs the GPU dearly
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

// brings u near the texture so it stays exact in float and int; past a
// texture either side the non-wrapping modes all read the same
float SampleWrap(float u, uint mode) {
    if (mode == 0u) return u - floor(u);
    if (mode == 1u) return u - 2.0f * floor(u * 0.5f);
    return u < -2.0f ? -2.0f : u > 2.0f ? 2.0f : u;
}

float4 SampleTap(SAMPLE_TEX, uint level, int x, int y, float4 border) {
    return x < 0 || y < 0 ? border : SAMPLE_LOAD(level, x, y);
}

float4 SampleLerp(float4 a, float4 b, float t) {
    return float4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

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
    // the short side clamped to a texel, as D3D11.3 7.18.11 has it, so a
    // magnified footprint takes one sample rather than blurring sub-texel
    // probes
    float n = 1.0f;
    if (ratio > 1u && pmax > 1.0f) n = min(ceil(pmax / max(pmin, 1.0f)), float(ratio));
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

// size is level 0's
float4 SampleTexture(SAMPLE_TEX, uint2 size, uint4 s, float2 uv, float2 dx, float2 dy) {
    const SamplePlan p = PlanSample(size, s, dx, dy);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float first = -0.5f * float(p.probes - 1u);
    // probes and levels in one loop, so the GPU code holds one copy of the
    // taps (it's inlined for five textures)
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
