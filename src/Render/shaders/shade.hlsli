// Experimental: RB3's material lighting, for both of the native view's
// backends. mesh.hlsl compiles it as HLSL; shade_model.cpp compiles it as C++
// for the CPU, with HLSL's vector types and functions from its own shim, so
// the two can't drift apart. Keep to what both languages share: float2/3/4
// built from every component, .x .y .z .w, the operators, the shim's
// functions (shade_model.cpp), `f` on float literals and SHADE_IN for a
// ShadeParams argument.
//
// The maths is out/research/m2_shader_ucode.md's, which read it from the
// game's shader microcode and checked it by running them; its Python models
// (tools/shaders/research: fam3.py standard, skin2.py skin, hair3.py hair) are
// the reference, and tests/shade_model_test.cpp checks this against them.
// Left out: the environment cube (not decoded), the projected light and the
// shadow buffer of a vertex-lit material, the hair's strand highlight (its
// colours, c2 and c19, along the bitangent) and normal maps in captures from
// before they kept the tangents.

float3 Xyz(float4 v) { return float3(v.x, v.y, v.z); }

// normalize, but zero stays zero instead of NaN, alike on both backends
float3 SafeNormalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20f)); }

// x^p for x in [0, 1]; 0 at 0, where HLSL's pow (exp2(p * log2 x)) and C's
// could disagree
float PowSat(float x, float p) { return x > 0.0f ? pow(x, p) : 0.0f; }

// The normal map's frame (NORMAL_MAP; out/research/m2_shader_ucode.md 6),
// as the game's vertex shaders build it (837915E757EEC6DC, skinned
// 18A3E6C52471D288): from the vertex's tangent T (w its handedness, +-1) and
// normal N, the bitangent B = T.w (N x T), then the frame turned by the
// texgen matrix, the tangent U = c20.x T + c20.y B + c20.z N and the normal
// N' = c22.x T + c22.y B + c22.z N (T and N where it's the identity), in the
// mesh's space. The backends turn both into the world as they do a normal
// (by the bones, or world) and the pixels take them interpolated, with the
// bitangent T.w (N'w x Uw) (Bitangent); none of them normalised.
struct TangentFrame {
    float3 n;  // N'
    float3 u;  // U
};

TangentFrame TextureFrame(SHADE_IN(ShadeParams) sp, float3 n, float4 t) {
    const float3 tt = Xyz(t);
    const float3 b = cross(n, tt) * t.w;
    TangentFrame f;
    f.u = tt * sp.texgen[0].x + b * sp.texgen[0].y + n * sp.texgen[0].z;
    f.n = tt * sp.texgen_n.x + b * sp.texgen_n.y + n * sp.texgen_n.z;
    return f;
}

// n and u in the world, w the tangent's handedness
float3 Bitangent(float3 n, float3 u, float w) { return cross(n, u) * w; }

// The normals a pixel lights with: `diffuse` for the diffuse light, the box
// map, the projected light and the shadow's diffuse darkening, `specular`
// for the specular, the point lights' rim and their shadow. Only the skin
// family's two differ.
struct Normals {
    float3 diffuse;
    float3 specular;
};

// n, u and b are the interpolated normal, tangent and bitangent (the frame's,
// with kShadeNormalMap), map s1's texel and detail s14's. The game's pixel
// shaders (7C6659287B361841, detail 74DC45137476D064; skin FF3F7B88EB727BF9):
// x = 2 s1.x - 1 pairs with the bitangent and y = 2 s1.y - 1 with the tangent,
// z = sat(1 - x^2 - y^2) (no square root) with the normal, and
//   N = normalize(z n + c14.x (x b + y u));
// the detail map adds c106.x of its own x, y and z to those first. The skin
// family lights its diffuse with that but without the detail map, and its
// specular with the detail map and without c14's softening.
Normals MappedNormals(SHADE_IN(ShadeParams) sp, float3 n, float3 u, float3 b, float4 map,
                      float4 detail) {
    const uint f = sp.flags.x;
    Normals o;
    o.diffuse = n;
    o.specular = n;
    if ((f & kShadeNormalMap) == 0u) return o;
    float x = map.x * 2.0f - 1.0f;
    float y = map.y * 2.0f - 1.0f;
    float z = saturate(1.0f - x * x - y * y);
    const float soft = sp.normal_map.x;
    const bool skin = (f & kShadeSkin) != 0u;
    if (skin) o.diffuse = SafeNormalize(n * z + (b * x + u * y) * soft);
    if ((f & kShadeDetailMap) != 0u) {
        const float dx = detail.x * 2.0f - 1.0f;
        const float dy = detail.y * 2.0f - 1.0f;
        const float share = sp.normal_map.y;
        x = x + share * dx;
        y = y + share * dy;
        z = z + share * saturate(1.0f - dx * dx - dy * dy);
    }
    if (skin) {
        o.specular = SafeNormalize(n * z + (b * x + u * y));
    } else {
        o.diffuse = SafeNormalize(n * z + (b * x + u * y) * soft);
        o.specular = o.diffuse;
    }
    return o;
}

// where the detail map is read: the texture's uv times c106.y
float2 DetailUv(SHADE_IN(ShadeParams) sp, float2 uv) {
    return float2(uv.x * sp.normal_map.z, uv.y * sp.normal_map.z);
}

// u' = c20.x u + c20.y v + c20.w, likewise v' with c21 (the game's VS)
float2 TexGen(SHADE_IN(ShadeParams) sp, float2 uv) {
    return float2(sp.texgen[0].x * uv.x + sp.texgen[0].y * uv.y + sp.texgen[0].w,
                  sp.texgen[1].x * uv.x + sp.texgen[1].y * uv.y + sp.texgen[1].w);
}

// A BILLBOARD draw's vertex turned to the camera (kShadeBillboard): the
// crowd's impostor quads, one DxMultiMesh instance each, lie in their mesh's
// XZ, and the game's vertex shaders (4B19F15CA3B46FEB lit, 7D050DB197258C07
// unlit; tools/shaders/research/crowd.py) place local v at
//   P = T + v.x R + v.y F + v.z U
// with R, U and F the camera's right, up and forward (VS c16..c18's
// columns) and T the instance's translation: its rotation and scale are left
// out. The normal turns the same way, without T. Returns the turned v.
float3 Billboard(SHADE_IN(ShadeParams) sp, float3 v) {
    return Xyz(sp.billboard[0]) * v.x + Xyz(sp.billboard[2]) * v.y + Xyz(sp.billboard[1]) * v.z;
}

// The box map lit along d: each face's colour weighted by sat(+-d.a), linear
float3 Box(SHADE_IN(ShadeParams) sp, float3 d) {
    return Xyz(sp.box[0]) * max(d.x, 0.0f) + Xyz(sp.box[1]) * max(-d.x, 0.0f) +
           Xyz(sp.box[2]) * max(d.y, 0.0f) + Xyz(sp.box[3]) * max(-d.y, 0.0f) +
           Xyz(sp.box[4]) * max(d.z, 0.0f) + Xyz(sp.box[5]) * max(-d.z, 0.0f);
}

// its specular: a Phong lobe per face on the reflection vector
float3 BoxSpecular(SHADE_IN(ShadeParams) sp, float3 r, float p) {
    return Xyz(sp.box[0]) * PowSat(saturate(r.x), p) + Xyz(sp.box[1]) * PowSat(saturate(-r.x), p) +
           Xyz(sp.box[2]) * PowSat(saturate(r.y), p) + Xyz(sp.box[3]) * PowSat(saturate(-r.y), p) +
           Xyz(sp.box[4]) * PowSat(saturate(r.z), p) + Xyz(sp.box[5]) * PowSat(saturate(-r.z), p);
}

// Where the projected light's maps (s5, s10) are read at world position p:
// PS c95..c97 projectively, as the game's pixel shaders do it (B8FEEA37CC970356
// instrs 4-8, a reciprocal and a multiply), unguarded behind the light. The
// backends sample there bilinearly, clamped to a transparent black border, as
// every capture's s5 fetch constant says.
float2 ProjUv(SHADE_IN(ShadeParams) sp, float3 p) {
    const float4 q = float4(p.x, p.y, p.z, 1.0f);
    const float iw = 1.0f / dot(sp.proj[2], q);
    return float2(dot(sp.proj[0], q) * iw, dot(sp.proj[1], q) * iw);
}

// The shadow buffer (SHADOW_BUFFER, kShadeShadow; out/research/
// m2_shader_ucode.md 5, the game's A46F95815504AFE1): RndShadowMap::PrepShadow
// draws the character's depth (clip z/w, 0 near, cleared to 1) from a light
// camera into a 512x512 map, and its pixels read s5 there. The coordinate is
// the VS's c40..c43 times the world position, which hold the light camera's
// view-projection times the texture's (u = .5x + .5009765625w, v = -.5y +
// .5009765625w, as the captures have them: half a texel on, so the taps'
// bilinear centre is the texel the map drew at that pixel, on D3D9's pixel
// centres), and the pixel shader divides it by its w.
float4 ShadowCoord(SHADE_IN(ShadeParams) sp, float3 p) {
    const float4 q = float4(p.x, p.y, p.z, 1.0f);
    return float4(dot(sp.shadow[0], q), dot(sp.shadow[1], q), dot(sp.shadow[2], q),
                  dot(sp.shadow[3], q));
}

// The four texels the game's shader reads, point-sampled at uv half a texel
// up-left, up-right, down-left and down-right (instrs 6-9), and the bilinear
// weights getWeights2D gives them (instr 10, 20): x = u size - 0.5, the taps
// floor(x) and floor(x) + 1, clamped to the map, weighted by x's fraction;
// likewise y. depth is the pixel's own, S.z / S.w.
struct ShadowTapSet {
    float4 x;       // columns: i, i + 1, i, i + 1
    float4 y;       // rows: j, j, j + 1, j + 1
    float4 weight;  // (1 - fx)(1 - fy), fx (1 - fy), (1 - fx) fy, fx fy
    float depth;
};

ShadowTapSet ShadowTaps(float4 s, float2 size) {
    const float iw = 1.0f / s.w;
    const float x = s.x * iw * size.x - 0.5f;
    const float y = s.y * iw * size.y - 0.5f;
    const float i = floor(x);
    const float j = floor(y);
    const float fx = x - i;
    const float fy = y - j;
    // clamped as floats, so a NaN (a pixel on the light's plane) lands on 0
    const float i0 = min(max(i, 0.0f), size.x - 1.0f);
    const float i1 = min(max(i + 1.0f, 0.0f), size.x - 1.0f);
    const float j0 = min(max(j, 0.0f), size.y - 1.0f);
    const float j1 = min(max(j + 1.0f, 0.0f), size.y - 1.0f);
    ShadowTapSet t;
    t.x = float4(i0, i1, i0, i1);
    t.y = float4(j0, j0, j1, j1);
    t.weight = float4((1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy);
    t.depth = s.z * iw;
    return t;
}

// How lit the pixel is, 0..1: each tap lit where the depth the map holds
// there is at or behind the pixel's (sge, instr 17), weighted (instr 22)
float ShadowLit(ShadowTapSet t, float4 stored) {
    const float4 lit = float4(stored.x >= t.depth ? 1.0f : 0.0f, stored.y >= t.depth ? 1.0f : 0.0f,
                              stored.z >= t.depth ? 1.0f : 0.0f, stored.w >= t.depth ? 1.0f : 0.0f);
    return dot(t.weight, lit);
}

// true if alpha test throws the pixel away
bool AlphaCut(SHADE_IN(ShadeParams) sp, float alpha) {
    return (sp.flags.x & kShadeAlphaCut) != 0u && alpha * 255.0f < sp.alpha_cut.x;
}

// The AO the game's shaders with a point light have (kShadeAoSh; out/
// research/m2_shader_ucode.md 3): the vertex colour holds the occlusion as
// spherical harmonics, red the constant term and alpha, green and blue the
// linear ones along x, y and z, and each point light is dimmed by what's
// visible toward it over what a bare surface would see (a second light by
// its own: VS 59D4812E2C2329A3, DFA2CEAD6EF11301). Their vertex shaders work
// it out per vertex and their pixels take it interpolated, as the backends
// do.
//
// The linear terms, in the mesh's own space. The backends turn them into the
// world as they do the normal (by the bones, or world), and don't normalise
// either: the game dots both raw with the light turned into the mesh's space
// by the transposed world matrix, which comes to the same.
float3 AoShDirection(float4 vc) {
    return float3(vc.w * 2.0f - 1.0f, vc.y * 2.0f - 1.0f, vc.z * 2.0f - 1.0f);
}

// visible over bare toward point light i from p, n and dir turned as above,
// r the vertex colour's red; 1 where bare is 0 or less
float AoShRatio(SHADE_IN(ShadeParams) sp, uint i, float3 p, float3 n, float3 dir, float r) {
    const float3 L = SafeNormalize(Xyz(sp.point_pos[i]) - p);
    const float vis = 0.282095f * r + 0.488603f * dot(dir, L);
    const float bare = 2.356194f * (0.079577f + 0.238732f * dot(n, L));
    return bare > 0.0f ? vis / bare : 1.0f;
}

// the point lights' occlusion at a vertex, for Light's ao_sh: x light 0's,
// y light 1's; 1 without kShadeAoSh or the light
float2 AoShVertex(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 dir, float4 vc) {
    float2 ao = float2(1.0f, 1.0f);
    if ((sp.flags.x & kShadeAoSh) == 0u) return ao;
    ao.x = saturate(1.0f + sp.ao.x * (AoShRatio(sp, 0u, p, n, dir, vc.x) - 1.0f));
    if (sp.flags.y >= 2u)
        ao.y = saturate(1.0f + sp.ao.x * (AoShRatio(sp, 1u, p, n, dir, vc.x) - 1.0f));
    return ao;
}

// A lit material's light at a point: its colour is base * diffuse + added,
// base the texture. The per-pixel shaders work it out per pixel; the
// vertex-lit ones (no PER_PIXEL) per vertex, and their pixels add up the
// interpolated two.
struct Lighting {
    float3 diffuse;  // times the texture
    float3 added;    // after it: specular, and the rim's own light
};

// p is the world position, n the world normal (any length) and n_spec the
// specular one (MappedNormals': n but for a normal-mapped skin), vc the
// vertex colour, spec_map the specular map's texel (1 where sp doesn't
// sample it),
// ao_sh the AoShVertex (interpolated, in a pixel), proj and gobo the
// projected light's maps' texels at ProjUv (s5 and s10; per pixel only, and
// unread where sp doesn't sample them), lit the shadow buffer's ShadowLit
// (per pixel only, unread without kShadeShadow)
Lighting Light(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 n_spec, float4 vc,
               float4 spec_map, float2 ao_sh, float4 proj, float4 gobo, float lit) {
    const uint f = sp.flags.x;
    const bool skin = (f & kShadeSkin) != 0u;
    const bool hair = (f & kShadeHair) != 0u;
    const bool box = (f & kShadeBox) != 0u;
    const float3 N = SafeNormalize(n);
    const float3 V = SafeNormalize(Xyz(sp.eye) - p);
    const float vn = saturate(dot(N, V));
    const float up = 0.5f * N.z + 0.5f;  // world Z is up
    // and the specular normal's (N's but for a normal-mapped skin)
    const float3 N2 = SafeNormalize(n_spec);
    const float nv2 = dot(N2, V);
    const float vn2 = saturate(nv2);
    const float3 R = N2 * (2.0f * nv2) - V;  // the eye's reflection
    const float up2 = 0.5f * N2.z + 0.5f;

    // ambient occlusion from the vertex colour's red: point light 0 gets the
    // stronger aoD, the rest of the light aoA; with a point light, the point
    // lights get their directional ones
    float ao_a = 1.0f;
    float ao_0 = 1.0f;  // point light 0's
    float ao_1 = 1.0f;  // point light 1's
    if ((f & kShadeAO) != 0u) {
        ao_a = saturate(1.0f + sp.ao.x * (1.128379f * vc.x - 1.0f));
        ao_0 = saturate(1.0f + sp.ao.x * (1.504505f * vc.x - 1.0f));
        ao_1 = ao_a;
        if ((f & kShadeAoSh) != 0u) {
            ao_0 = ao_sh.x;
            ao_1 = ao_sh.y;
        }
    }

    float power = sp.specular.w;
    float3 spec_color = Xyz(sp.specular);
    if ((f & kShadeSpecMap) != 0u) {
        power = max(spec_map.w * sp.specular.w, 0.5f);
        spec_color = spec_color * Xyz(spec_map);
    }
    const float spec_norm = power * 0.159155f + 0.31831f;  // (p + 2) / 2 pi
    const bool rim = (f & kShadeRim) != 0u;
    const float rim_b = PowSat(1.0f - vn2 * vn2, sp.rim.w);

    // the skin and hair families wrap the diffuse, per channel
    float3 wrap_a = float3(0.55f, 0.6f, 0.65f);
    float3 wrap_b = float3(0.45f, 0.4f, 0.35f);
    if (hair) {
        wrap_a = float3(0.75f, 0.8f, 0.85f);
        wrap_b = float3(0.25f, 0.2f, 0.15f);
    }

    const float3 zero = float3(0.0f, 0.0f, 0.0f);
    const float3 one = float3(1.0f, 1.0f, 1.0f);

    // The projected light (NUM_PROJ; m2_shader_ucode.md 5, fam3.py). The
    // multiply form darkens the box map, the point lights, their specular
    // and the rim by up to 0.75 c69 where s5's alpha is, as far as the
    // surface faces it, but not the ambient c1. The gobo form adds c69 times
    // s10's colour where s5's alpha leaves it, occluded by the VS's ao.y:
    // light 1's (SH, with two) or aoA (B3ACFB8F2C183C5B, 7928EF7EADF085F6).
    float3 proj_mul = one;
    float3 proj_add = zero;
    if ((f & (kShadeProjMultiply | kShadeProjGobo)) != 0u) {
        const float facing = saturate(dot(N, Xyz(sp.proj_dir)));
        if ((f & kShadeProjMultiply) != 0u) {
            proj_mul = one - Xyz(sp.proj_color) * (0.75f * proj.w * facing);
        } else {
            const float ao_proj = sp.flags.y >= 2u ? ao_1 : ao_a;
            proj_add = Xyz(sp.proj_color) * Xyz(gobo) * ((1.0f - proj.w) * facing * ao_proj);
        }
    }

    // The shadow buffer (A46F95815504AFE1 instrs 15-29): where the map has
    // the character nearer the light than the pixel, every point light, its
    // specular and its rim are darkened by up to 0.75 c107 (1 - the shadow's
    // colour), as far as the surface faces away from the light camera's
    // forward c108; not the ambient, the box map or the projected light's
    // gobo. The specular and the rim take it by the specular normal.
    float3 shadow = one;
    float3 shadow2 = one;
    if ((f & kShadeShadow) != 0u) {
        const float away = saturate(-dot(N, Xyz(sp.shadow_dir)));
        shadow = one - Xyz(sp.shadow_color) * (0.75f * away * (1.0f - lit));
        const float away2 = saturate(-dot(N2, Xyz(sp.shadow_dir)));
        shadow2 = one - Xyz(sp.shadow_color) * (0.75f * away2 * (1.0f - lit));
    }

    float3 lights = proj_add;  // the point lights' diffuse, and the gobo's
    float3 lights_spec = zero;
    float3 lights_rim = zero;
    for (uint i = 0u; i < 2u; i++) {
        if (i >= sp.flags.y) break;
        const float3 to_light = Xyz(sp.point_pos[i]) - p;
        const float d = sqrt(dot(to_light, to_light));
        const float3 L = to_light / max(d, 1e-6f);
        const float att = saturate(d * sp.point_pos[i].w + sp.point_color[i].w);
        const float3 lc_own =
            Xyz(sp.point_color[i]) * proj_mul * (att * (i == 0u ? ao_0 : ao_1));
        const float3 lc = lc_own * shadow;    // for the diffuse
        const float3 lc2 = lc_own * shadow2;  // the specular and the rim
        // a billboard's light is the falloff alone (4B19F15CA3B46FEB instrs
        // 37-40): its quad faces the camera, not the light
        const float nl = (f & kShadeBillboard) != 0u ? 1.0f : dot(N, L);
        if (skin || hair) {
            lights = lights + lc * saturate(wrap_a * nl + wrap_b);
        } else {
            lights = lights + lc * saturate(nl);
        }
        const float rl = saturate(dot(R, L));
        if (skin) {
            const float soft = 1.0f - vn2 * vn2;
            lights_spec = lights_spec + lc2 * (PowSat(rl, power) * soft * soft * spec_norm);
            if (rim) lights_rim = lights_rim + lc2 * (saturate(-dot(L, V)) * rim_b * up2);
        } else if (!hair) {
            // (the hair's highlight runs along the strands, in colours of
            // its own: left out)
            lights_spec = lights_spec + lc2 * (PowSat(rl, power) * spec_norm);
            if (rim) {
                const float under = (f & kShadeRimUnder) != 0u ? up2 : 1.0f;
                lights_rim = lights_rim + lc2 * (saturate(-dot(L, V)) * rim_b * under);
            }
        }
    }

    const float3 c0 = Xyz(sp.color);
    const float3 c1 = Xyz(sp.ambient);
    const float3 box_n = box ? Box(sp, N) * proj_mul : zero;
    const float rim_a = PowSat((1.0f - vn) * up, sp.rim.w);
    Lighting l;
    if (skin) {
        float3 amb = box_n * ao_a;
        if (rim && box) amb = amb * (one + Box(sp, -V) * rim_a);
        l.diffuse = (lights + amb) * c0 + c1 * c0 * ao_a;
        if (rim) l.diffuse = l.diffuse * (one + Xyz(sp.rim) * lights_rim);
    } else if ((f & kShadePrelit) != 0u) {
        l.diffuse = c0 * (box_n + lights) + Xyz(vc) * c1;
    } else {
        l.diffuse = c0 * ((c1 + box_n) * ao_a + lights);
    }
    l.added = zero;

    if ((f & kShadeSpecular) != 0u) {
        const float3 box_r = box ? BoxSpecular(sp, R, power) * proj_mul : zero;
        if (skin) {
            const float fs = (1.0f - vn2) * up2;
            l.added = spec_color * (lights_spec + box_r * (fs * fs * spec_norm * ao_a));
        } else {
            const float fresnel = (1.0f - vn2) * up2 + 0.25f;
            const float3 s = lights_spec + box_r * (fresnel * spec_norm * ao_a);
            // the hair's colour is its map's alone
            l.added = s * (hair ? Xyz(spec_map) : spec_color);
        }
    }
    if (rim && !skin && !hair) {
        // rgb = diffuse base (1 + 0.3 rim) + 0.7 rim
        const float3 rim_box = box ? Box(sp, -V) * proj_mul * (rim_a * ao_a) : zero;
        const float3 r = Xyz(sp.rim) * (rim_box + lights_rim);
        l.diffuse = l.diffuse * (one + r * 0.3f);
        l.added = l.added + r * 0.7f;
    }
    return l;
}

// One pixel's colour and alpha. p is its world position, n its interpolated
// world normal and u and b its tangent and bitangent (TextureFrame's and
// Bitangent's, read with kShadeNormalMap), vc its vertex colour, depth its
// clip w; texel, spec_map and glow are the maps' texels where sp samples
// them (1 where it doesn't), normal and detail the normal map's and the
// detail map's (MappedNormals'), behind the post-processed picture at the
// pixel for kShadeRefract; ao_sh is the interpolated AoShVertex, proj and
// gobo the projected light's maps' texels at ProjUv (Light's), lit the
// shadow buffer's ShadowLit (Light's), vertex the interpolated Lighting of a
// vertex-lit material's vertices.
float4 ShadePixel(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 u, float3 b, float4 vc,
                  float4 texel, float4 spec_map, float4 glow, float4 normal, float4 detail,
                  float4 behind, float depth, float2 ao_sh, float4 proj, float4 gobo, float lit,
                  Lighting vertex) {
    const uint f = sp.flags.x;
    // REFRACT_WORLD's pixel shader (FC53125B5EB914F8): the texture's rgb
    // times the picture behind it, alpha the texture's. The game nudges where
    // it reads the picture by a second map (s1 * 2 - 1, times c119.w); that's
    // left out, so it reads straight behind.
    if ((f & kShadeRefract) != 0u) {
        texel = float4(texel.x * behind.x, texel.y * behind.y, texel.z * behind.z, texel.w);
    }
    if ((f & kShadeModel) == 0u) {
        // the placeholder from before: colour times texture, times the vertex
        // colour if prelit, else a fixed light from above (Milo is z up)
        float4 c = sp.color * texel;
        if ((f & kShadePrelit) != 0u) {
            c = c * vc;
        } else if ((f & kShadeLegacyLight) != 0u) {
            const float len = sqrt(dot(n, n));
            float d = 0.0f;
            if (len > 1e-6f) d = dot(n, float3(0.39f, -0.59f, 0.71f)) / len;
            const float l = 0.4f + 0.6f * max(0.0f, d);
            c = float4(c.x * l, c.y * l, c.z * l, c.w);
        }
        return c;
    }

    float3 base = Xyz(texel);
    float base_alpha = texel.w;
    if ((f & kShadeIntensify) != 0u) {
        base = base * sp.emissive.y;
        base_alpha = base_alpha * sp.emissive.y;
    }
    float3 rgb;
    float alpha;
    if ((f & kShadeLit) == 0u) {
        // the unlit shaders: the VS's colour c0 c1 (times the vertex colour
        // if prelit, as particles are) times the texture
        float4 col = sp.color * sp.ambient;
        if ((f & kShadePrelit) != 0u) col = col * vc;
        rgb = base * Xyz(col);
        alpha = base_alpha * col.w;
    } else {
        Lighting l = vertex;
        if ((f & kShadePerVertex) == 0u) {
            const Normals nn = MappedNormals(sp, n, u, b, normal, detail);
            l = Light(sp, p, nn.diffuse, nn.specular, vc, spec_map, ao_sh, proj, gobo, lit);
        }
        rgb = base * l.diffuse + l.added;
        alpha = base_alpha * sp.ambient.w * ((f & kShadePrelit) != 0u ? vc.w : sp.color.w);
    }

    if ((f & kShadeGlow) != 0u) rgb = rgb + Xyz(glow) * sp.emissive.x;
    if ((f & kShadePseudoHdr) != 0u) alpha = dot(rgb, Xyz(sp.bloom));

    if ((f & (kShadeFadeAlpha | kShadeFadeColor)) != 0u) {
        // toward c104 with view depth, and between the left and right planes
        const float4 q = float4(p.x, p.y, p.z, 1.0f);
        const float fade = sp.fade[2].z * saturate((sp.fade[2].x - depth) * sp.fade[2].y) *
                           saturate(dot(sp.fade[0], q)) * saturate(dot(sp.fade[1], q));
        if ((f & kShadeFadeAlpha) != 0u) {
            alpha = lerp(sp.fade_color.w, alpha, fade);
        } else {
            rgb = lerp(Xyz(sp.fade_color), rgb, fade);
        }
    }
    return float4(rgb.x, rgb.y, rgb.z, alpha);
}

// A soft particle (scene_capture.h's IsSoftParticle; the game's pixel shader
// 66C00A7A56838997, out/research/softparticle_survey.md 2) is the particle's
// colour with its alpha times SoftFade: it fades out over the last 48 units
// of view depth before it meets the scene behind it. The game reads that
// scene depth back from its depth buffer (s9, point-sampled where the pixel
// is on the screen) by the camera's range, c89:
//   Zs = near far / (far - ((1 - s9) c89.z - c89.w) (far - near)),
// which is the view depth the scene was drawn at, and the far plane where
// nothing drew. The native depth is 1/w (inv_w, 0 where nothing drew).
float SoftSceneDepth(float far_plane, float inv_w) {
    return inv_w > 0.0f ? 1.0f / inv_w : far_plane;
}

// scene_depth SoftSceneDepth's, w the particle's own view depth (clip w)
float SoftFade(float scene_depth, float w) { return saturate((scene_depth - w) / 48.0f); }
