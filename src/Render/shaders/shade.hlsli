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
// (out/research/m2_shaders/tools: fam3.py standard, skin2.py skin, hair3.py
// hair) are the reference, and tests/shade_model_test.cpp checks this against
// them. Left out: normal maps (the capture has no tangents), the environment
// cube (not decoded), the shadow buffer (k_24_8; read as lit), the projected
// light and the hair's strand highlight (it needs the tangent).

float3 Xyz(float4 v) { return float3(v.x, v.y, v.z); }

// normalize, but zero stays zero instead of NaN, alike on both backends
float3 SafeNormalize(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20f)); }

// x^p for x in [0, 1]; 0 at 0, where HLSL's pow (exp2(p * log2 x)) and C's
// could disagree
float PowSat(float x, float p) { return x > 0.0f ? pow(x, p) : 0.0f; }

// u' = c20.x u + c20.y v + c20.w, likewise v' with c21 (the game's VS)
float2 TexGen(SHADE_IN(ShadeParams) sp, float2 uv) {
    return float2(sp.texgen[0].x * uv.x + sp.texgen[0].y * uv.y + sp.texgen[0].w,
                  sp.texgen[1].x * uv.x + sp.texgen[1].y * uv.y + sp.texgen[1].w);
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

// true if alpha test throws the pixel away
bool AlphaCut(SHADE_IN(ShadeParams) sp, float alpha) {
    return (sp.flags.x & kShadeAlphaCut) != 0u && alpha * 255.0f < sp.alpha_cut.x;
}

// A lit material's light at a point: its colour is base * diffuse + added,
// base the texture. The per-pixel shaders work it out per pixel; the
// vertex-lit ones (no PER_PIXEL) per vertex, and their pixels add up the
// interpolated two.
struct Lighting {
    float3 diffuse;  // times the texture
    float3 added;    // after it: specular, and the rim's own light
};

// p is the world position, n the world normal (any length), vc the vertex
// colour, spec_map the specular map's texel (1 where sp doesn't sample it)
Lighting Light(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float4 vc, float4 spec_map) {
    const uint f = sp.flags.x;
    const bool skin = (f & kShadeSkin) != 0u;
    const bool hair = (f & kShadeHair) != 0u;
    const bool box = (f & kShadeBox) != 0u;
    const float3 N = SafeNormalize(n);
    const float3 V = SafeNormalize(Xyz(sp.eye) - p);
    const float nv = dot(N, V);
    const float vn = saturate(nv);
    const float3 R = N * (2.0f * nv) - V;  // the eye's reflection
    const float up = 0.5f * N.z + 0.5f;    // world Z is up

    // ambient occlusion from the vertex colour's red: point light 0 gets the
    // stronger aoD, the rest of the light aoA
    float ao_a = 1.0f;
    float ao_d = 1.0f;
    if ((f & kShadeAO) != 0u) {
        ao_a = saturate(1.0f + sp.ao.x * (1.128379f * vc.x - 1.0f));
        ao_d = saturate(1.0f + sp.ao.x * (1.504505f * vc.x - 1.0f));
    }

    float power = sp.specular.w;
    float3 spec_color = Xyz(sp.specular);
    if ((f & kShadeSpecMap) != 0u) {
        power = max(spec_map.w * sp.specular.w, 0.5f);
        spec_color = spec_color * Xyz(spec_map);
    }
    const float spec_norm = power * 0.159155f + 0.31831f;  // (p + 2) / 2 pi
    const bool rim = (f & kShadeRim) != 0u;
    const float rim_b = PowSat(1.0f - vn * vn, sp.rim.w);

    // the skin and hair families wrap the diffuse, per channel
    float3 wrap_a = float3(0.55f, 0.6f, 0.65f);
    float3 wrap_b = float3(0.45f, 0.4f, 0.35f);
    if (hair) {
        wrap_a = float3(0.75f, 0.8f, 0.85f);
        wrap_b = float3(0.25f, 0.2f, 0.15f);
    }

    const float3 zero = float3(0.0f, 0.0f, 0.0f);
    float3 lights = zero;  // the point lights' diffuse
    float3 lights_spec = zero;
    float3 lights_rim = zero;
    for (uint i = 0u; i < 2u; i++) {
        if (i >= sp.flags.y) break;
        const float3 to_light = Xyz(sp.point_pos[i]) - p;
        const float d = sqrt(dot(to_light, to_light));
        const float3 L = to_light / max(d, 1e-6f);
        const float att = saturate(d * sp.point_pos[i].w + sp.point_color[i].w);
        const float3 lc = Xyz(sp.point_color[i]) * (att * (i == 0u ? ao_d : ao_a));
        const float nl = dot(N, L);
        if (skin || hair) {
            lights = lights + lc * saturate(wrap_a * nl + wrap_b);
        } else {
            lights = lights + lc * saturate(nl);
        }
        const float rl = saturate(dot(R, L));
        if (skin) {
            const float soft = 1.0f - vn * vn;
            lights_spec = lights_spec + lc * (PowSat(rl, power) * soft * soft * spec_norm);
            if (rim) lights_rim = lights_rim + lc * (saturate(-dot(L, V)) * rim_b * up);
        } else if (!hair) {
            // (the hair's highlight runs along the strands, the tangent the
            // capture doesn't have: left out)
            lights_spec = lights_spec + lc * (PowSat(rl, power) * spec_norm);
            if (rim) {
                const float under = (f & kShadeRimUnder) != 0u ? up : 1.0f;
                lights_rim = lights_rim + lc * (saturate(-dot(L, V)) * rim_b * under);
            }
        }
    }

    const float3 c0 = Xyz(sp.color);
    const float3 c1 = Xyz(sp.ambient);
    const float3 one = float3(1.0f, 1.0f, 1.0f);
    const float3 box_n = box ? Box(sp, N) : zero;
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
        const float3 box_r = box ? BoxSpecular(sp, R, power) : zero;
        if (skin) {
            const float fs = (1.0f - vn) * up;
            l.added = spec_color * (lights_spec + box_r * (fs * fs * spec_norm * ao_a));
        } else {
            const float fresnel = (1.0f - vn) * up + 0.25f;
            const float3 s = lights_spec + box_r * (fresnel * spec_norm * ao_a);
            // the hair's colour is its map's alone
            l.added = s * (hair ? Xyz(spec_map) : spec_color);
        }
    }
    if (rim && !skin && !hair) {
        // rgb = diffuse base (1 + 0.3 rim) + 0.7 rim
        const float3 rim_box = box ? Box(sp, -V) * (rim_a * ao_a) : zero;
        const float3 r = Xyz(sp.rim) * (rim_box + lights_rim);
        l.diffuse = l.diffuse * (one + r * 0.3f);
        l.added = l.added + r * 0.7f;
    }
    return l;
}

// One pixel's colour and alpha. p is its world position, n its interpolated
// world normal, vc its vertex colour, depth its clip w; texel, spec_map and
// glow are the maps' texels where sp samples them (1 where it doesn't);
// vertex is the interpolated Lighting of a vertex-lit material's vertices.
float4 ShadePixel(SHADE_IN(ShadeParams) sp, float3 p, float3 n, float4 vc, float4 texel,
                  float4 spec_map, float4 glow, float depth, Lighting vertex) {
    const uint f = sp.flags.x;
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
        if ((f & kShadePerVertex) == 0u) l = Light(sp, p, n, vc, spec_map);
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
