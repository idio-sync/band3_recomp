// RB3's material lighting, shared by both backends: mesh.hlsl compiles it as
// HLSL, shade_model.cpp as C++ through its shim. Keep to what both languages
// share: float2/3/4 built from every component, .x .y .z .w, the operators,
// the shim's functions, `f` on float literals, SHADE_IN for a ShadeParams.
//
// From out/research/m2_shader_ucode.md; the reference models are
// tools/shaders/research (fam3.py, skin2.py, hair3.py), checked by
// tests/shade_model_test.cpp. Left out: the environment cube, a vertex-lit
// material's projected light and shadow buffer, and normal maps (and so the
// hair's strand highlight) in captures without tangents.

float3 Xyz(float4 v) { return float3(v.x, v.y, v.z); }

// normalize, but zero stays zero instead of NaN, alike on both backends
float3 SafeNormalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20f)); }

// x^p for x in [0, 1]; 0 at 0, where HLSL's pow and C's could disagree
float PowSat(float x, float p) { return x > 0.0f ? pow(x, p) : 0.0f; }

// The normal map's frame in mesh space (NORMAL_MAP; m2_shader_ucode.md 6;
// VS 837915E757EEC6DC, skinned 18A3E6C52471D288): T the tangent (w its
// handedness), B = T.w (N x T), U = c20.x T + c20.y B + c20.z N,
// N' = c22.x T + c22.y B + c22.z N. The backends turn both to the world like a
// normal; pixels rebuild the bitangent with Bitangent. None normalised.
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

// `specular` lights the specular and rim and their shadow, `diffuse` the rest.
// Only the skin family's two differ.
struct Normals {
    float3 diffuse;
    float3 specular;
};

// map is s1's texel, detail s14's (PS 7C6659287B361841, detail
// 74DC45137476D064, skin FF3F7B88EB727BF9):
//   N = normalize(z n + c14.x (x b + y u)), x = 2 s1.x - 1, y = 2 s1.y - 1,
//   z = sat(1 - x^2 - y^2) (no square root);
// the detail map adds c106.x of its own x, y, z first. Skin's diffuse skips
// the detail map; its specular skips c14's softening.
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

// uv times c106.y
float2 DetailUv(SHADE_IN(ShadeParams) sp, float2 uv) {
    return float2(uv.x * sp.normal_map.z, uv.y * sp.normal_map.z);
}

// u' = c20.x u + c20.y v + c20.w, likewise v' with c21 (the game's VS)
float2 TexGen(SHADE_IN(ShadeParams) sp, float2 uv) {
    return float2(sp.texgen[0].x * uv.x + sp.texgen[0].y * uv.y + sp.texgen[0].w,
                  sp.texgen[1].x * uv.x + sp.texgen[1].y * uv.y + sp.texgen[1].w);
}

// A BILLBOARD vertex turned to the camera (kShadeBillboard; the crowd's
// impostor quads, in their mesh's XZ). VS 4B19F15CA3B46FEB lit,
// 7D050DB197258C07 unlit (crowd.py): P = T + v.x R + v.y F + v.z U, R U F the
// camera's right, up, forward (VS c16..c18's columns), T the instance's
// translation; its rotation and scale are ignored. Returns v turned, without T.
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

// The projected light's maps' (s5, s10) uv at world p: PS c95..c97
// projectively (B8FEEA37CC970356 instrs 4-8), unguarded behind the light.
// Sampled bilinear with a transparent black border, per s5's fetch constant.
float2 ProjUv(SHADE_IN(ShadeParams) sp, float3 p) {
    const float4 q = float4(p.x, p.y, p.z, 1.0f);
    const float iw = 1.0f / dot(sp.proj[2], q);
    return float2(dot(sp.proj[0], q) * iw, dot(sp.proj[1], q) * iw);
}

// The shadow buffer's coordinate (SHADOW_BUFFER; m2_shader_ucode.md 5,
// A46F95815504AFE1): RndShadowMap::PrepShadow draws the character's depth
// (clip z/w, 0 near, cleared to 1) from a light camera into a 512x512 s5.
// VS c40..c43 is the light's view-projection times u = .5x + .5009765625w,
// v = -.5y + .5009765625w (half a texel on, for D3D9's pixel centres); the
// pixel divides by w.
float4 ShadowCoord(SHADE_IN(ShadeParams) sp, float3 p) {
    const float4 q = float4(p.x, p.y, p.z, 1.0f);
    return float4(dot(sp.shadow[0], q), dot(sp.shadow[1], q), dot(sp.shadow[2], q),
                  dot(sp.shadow[3], q));
}

// The game's four point-sampled taps (instrs 6-9) and getWeights2D's bilinear
// weights (instrs 10, 20), clamped to the map. depth is the pixel's, S.z / S.w.
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

// 0..1; a tap is lit where the map's depth >= the pixel's (sge, instr 17)
float ShadowLit(ShadowTapSet t, float4 stored) {
    const float4 lit = float4(stored.x >= t.depth ? 1.0f : 0.0f, stored.y >= t.depth ? 1.0f : 0.0f,
                              stored.z >= t.depth ? 1.0f : 0.0f, stored.w >= t.depth ? 1.0f : 0.0f);
    return dot(t.weight, lit);
}

bool AlphaCut(SHADE_IN(ShadeParams) sp, float alpha) {
    return (sp.flags.x & kShadeAlphaCut) != 0u && alpha * 255.0f < sp.alpha_cut.x;
}

// Point-light AO (kShadeAoSh; m2_shader_ucode.md 3; VS 59D4812E2C2329A3,
// DFA2CEAD6EF11301): the vertex colour holds spherical harmonics, red the
// constant term, alpha green blue the linear x y z; each point light is
// dimmed by visible over bare toward it. Per vertex, interpolated.
//
// The linear terms in mesh space; turned to the world like the normal and,
// as in the game, not normalised.
float3 AoShDirection(float4 vc) {
    return float3(vc.w * 2.0f - 1.0f, vc.y * 2.0f - 1.0f, vc.z * 2.0f - 1.0f);
}

// r the vertex colour's red; 1 where bare <= 0
float AoShRatio(SHADE_IN(ShadeParams) sp, uint i, float3 p, float3 n, float3 dir, float r) {
    const float3 L = SafeNormalize(Xyz(sp.point_pos[i]) - p);
    const float vis = 0.282095f * r + 0.488603f * dot(dir, L);
    const float bare = 2.356194f * (0.079577f + 0.238732f * dot(n, L));
    return bare > 0.0f ? vis / bare : 1.0f;
}

// Light's ao_sh: x light 0's, y light 1's; 1 without kShadeAoSh or the light
float2 AoShVertex(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 dir, float4 vc) {
    float2 ao = float2(1.0f, 1.0f);
    if ((sp.flags.x & kShadeAoSh) == 0u) return ao;
    ao.x = saturate(1.0f + sp.ao.x * (AoShRatio(sp, 0u, p, n, dir, vc.x) - 1.0f));
    if (sp.flags.y >= 2u)
        ao.y = saturate(1.0f + sp.ao.x * (AoShRatio(sp, 1u, p, n, dir, vc.x) - 1.0f));
    return ao;
}

// colour = texture * diffuse + added; per vertex without PER_PIXEL
struct Lighting {
    float3 diffuse;  // times the texture
    float3 added;    // after it: specular, and the rim's own light
};

// World p; n and n_spec any length (MappedNormals'); spec_map 1 where not
// sampled; proj and gobo s5 and s10 at ProjUv and lit ShadowLit (per pixel
// only); strand the unnormalised bitangent (kShadeHair with kShadeNormalMap)
Lighting Light(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 n_spec, float4 vc,
               float4 spec_map, float2 ao_sh, float4 proj, float4 gobo, float lit,
               float3 strand) {
    const uint f = sp.flags.x;
    const bool skin = (f & kShadeSkin) != 0u;
    const bool hair = (f & kShadeHair) != 0u;
    const bool box = (f & kShadeBox) != 0u;
    const float3 N = SafeNormalize(n);
    const float3 V = SafeNormalize(Xyz(sp.eye) - p);
    const float vn = saturate(dot(N, V));
    const float up = 0.5f * N.z + 0.5f;  // world Z is up
    const float3 N2 = SafeNormalize(n_spec);
    const float nv2 = dot(N2, V);
    const float vn2 = saturate(nv2);
    const float3 R = N2 * (2.0f * nv2) - V;
    const float up2 = 0.5f * N2.z + 0.5f;

    // AO from the vertex colour's red: point light 0 gets the stronger aoD,
    // the rest aoA; kShadeAoSh replaces the point lights'
    float ao_a = 1.0f;
    float ao_0 = 1.0f;
    float ao_1 = 1.0f;
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

    // The hair's strand highlight (hair3.py; 47D5A8EE975C7558 instrs 62-80):
    // where R crosses the strands (the bitangent), c2 on the core and c19 on
    // the sheen; the point lights' specular is it times sat(L.R). Skipped
    // without the frame.
    float3 strand_color = zero;
    if (hair && (f & kShadeNormalMap) != 0u) {
        const float rt = dot(R, strand);
        const float s = abs(1.0f - rt * rt);
        const float a2 = PowSat(s, 2.0f * power);
        const float a4 = PowSat(s, 4.0f * power);
        strand_color = Xyz(sp.specular) * a4 + Xyz(sp.specular2) * (a2 * (1.0f - a4));
    }

    // The projected light (NUM_PROJ; m2_shader_ucode.md 5, fam3.py). Multiply
    // darkens all but the ambient c1 by up to 0.75 c69 by s5's alpha and
    // facing. Gobo adds c69 times s10 where s5's alpha leaves it, occluded by
    // the VS's ao.y: light 1's or aoA (B3ACFB8F2C183C5B, 7928EF7EADF085F6).
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

    // The shadow buffer (A46F95815504AFE1 instrs 15-29) darkens the point
    // lights by up to 0.75 c107 (1 - shadow colour) as the surface faces away
    // from c108, the light camera's forward; specular and rim by N2.
    float3 shadow = one;
    float3 shadow2 = one;
    if ((f & kShadeShadow) != 0u) {
        const float away = saturate(-dot(N, Xyz(sp.shadow_dir)));
        shadow = one - Xyz(sp.shadow_color) * (0.75f * away * (1.0f - lit));
        const float away2 = saturate(-dot(N2, Xyz(sp.shadow_dir)));
        shadow2 = one - Xyz(sp.shadow_color) * (0.75f * away2 * (1.0f - lit));
    }

    float3 lights = proj_add;
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
        const float3 lc = lc_own * shadow;
        const float3 lc2 = lc_own * shadow2;
        // a billboard's light is the falloff alone (4B19F15CA3B46FEB 37-40)
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
        } else if (hair) {
            lights_spec = lights_spec + lc2 * strand_color * (rl * spec_norm);
        } else {
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
            // hair's colour is its map's alone; the strands' is in s
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

// A movie's YUV texels to RGB (PS 22F426E8D3A1F1B5, ShaderType 11; movie.py):
// Bink's BT.601 with offsets, the shader's c254/c255 literals exactly.
// Unclamped; the target clamps.
float4 MovieRgb(float y, float cr, float cb) {
    const float a = 1.16412353515625f * y;
    return float4(a + 1.595794677734375f * cr - 0.8706550598144531f,
                  a - 0.8134765625f * cr - 0.391448974609375f * cb + 0.5297050476074219f,
                  a + 2.017822265625f * cb - 1.0816688537597656f, 1.0f);
}

// Where REFRACT_WORLD (PS FC53125B5EB914F8; refract.py) reads the picture
// behind, 0..1: uv = (clip.xy + offset) / w * (0.5, -0.5) + 0.5, offset
// c119.w times s1 * 2 - 1, green across and red down. Read bilinear, clamped
// (NgMat::SetupShader's s6).
float2 RefractUv(SHADE_IN(ShadeParams) sp, float2 clip, float w, float4 map) {
    float2 c = clip;
    if ((sp.flags.x & kShadeRefractMap) != 0u) {
        const float k = sp.refract.x;
        c = float2(c.x + k * (map.y * 2.0f - 1.0f), c.y + k * (map.x * 2.0f - 1.0f));
    }
    return float2(c.x / w * 0.5f + 0.5f, c.y / w * -0.5f + 0.5f);
}

// World p, n, u, b; depth is clip w; maps not sampled are 1; behind is the
// picture at RefractUv; vertex the interpolated Lighting of a vertex-lit
// material. Other arguments as Light's.
float4 ShadePixel(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float3 u, float3 b, float4 vc,
                  float4 texel, float4 spec_map, float4 glow, float4 normal, float4 detail,
                  float4 behind, float depth, float2 ao_sh, float4 proj, float4 gobo, float lit,
                  Lighting vertex) {
    const uint f = sp.flags.x;
    if ((f & kShadeYuv) != 0u) {
        // a chroma plane the backend doesn't have is Bink's neutral, 128
        const float neutral = 128.0f / 255.0f;
        return MovieRgb(texel.x, (f & kShadeSpecMap) != 0u ? spec_map.x : neutral,
                        (f & kShadeGlow) != 0u ? glow.x : neutral);
    }
    if ((f & kShadeRefract) != 0u) {
        texel = float4(texel.x * behind.x, texel.y * behind.y, texel.z * behind.z, texel.w);
    }
    if ((f & kShadeModel) == 0u) {
        // placeholder; Milo is z up
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
        // unlit: the VS's c0 c1
        float4 col = sp.color * sp.ambient;
        if ((f & kShadePrelit) != 0u) col = col * vc;
        rgb = base * Xyz(col);
        alpha = base_alpha * col.w;
    } else {
        Lighting l = vertex;
        if ((f & kShadePerVertex) == 0u) {
            const Normals nn = MappedNormals(sp, n, u, b, normal, detail);
            l = Light(sp, p, nn.diffuse, nn.specular, vc, spec_map, ao_sh, proj, gobo, lit, b);
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

// A soft particle (IsSoftParticle; PS 66C00A7A56838997,
// softparticle_survey.md 2) fades its alpha over the last 48 units of view
// depth before the scene. The game reads s9's depth back by c89; the native
// depth is 1/w, 0 where nothing drew (then the far plane).
float SoftSceneDepth(float far_plane, float inv_w) {
    return inv_w > 0.0f ? 1.0f / inv_w : far_plane;
}

// w is the particle's clip w
float SoftFade(float scene_depth, float w) { return saturate((scene_depth - w) / 48.0f); }
