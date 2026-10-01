// Experimental: what the native view's shading reads for one draw, packed
// from its ShadeState by PackShade (shade_model.cpp). Shared by mesh.hlsl,
// where it's part of the pixel uniforms, and the CPU (shade_model.h includes
// it as C++, with float4 and uint4 its own), so both backends read the same
// numbers. Everything is a float4 or uint4: HLSL's cbuffer packing and C++'s
// layout are then the same.

// flags.x
static const uint kShadeModel = 1u;        // RB3's shading; without it, the placeholder
static const uint kShadeTextured = 2u;     // the diffuse texture is sampled
static const uint kShadePrelit = 4u;       // the vertex colour is baked light (or particles')
static const uint kShadeLit = 8u;          // REAL_LIGHTS or APPROX_LIGHTS: the lit shaders
static const uint kShadeBox = 16u;         // APPROX_LIGHTS: the box map
static const uint kShadeSpecular = 32u;
static const uint kShadeSpecMap = 64u;     // s2, sampled
static const uint kShadeGlow = 128u;       // s3, sampled
static const uint kShadeAO = 256u;         // ENABLE_AO: the vertex colour's red is occlusion
static const uint kShadeIntensify = 512u;  // texture rgb and alpha times c5.y
static const uint kShadePseudoHdr = 1024u; // alpha is the bloom luminance
static const uint kShadeRim = 2048u;       // RIM_LIGHT
static const uint kShadeRimUnder = 4096u;  // RIM_UNDER: the point rim fades toward -Z
static const uint kShadeSkin = 8192u;      // shader_variation 1's family
static const uint kShadeHair = 16384u;     // shader_variation 2's family
static const uint kShadeFadeAlpha = 32768u;   // FADE_OUT 1
static const uint kShadeFadeColor = 65536u;   // FADE_OUT 2
static const uint kShadeAlphaCut = 131072u;
// the placeholder's simple directional light (RasterOptions::lighting)
static const uint kShadeLegacyLight = 262144u;
// no PER_PIXEL: lit per vertex, Light() in the vertex shader
static const uint kShadePerVertex = 524288u;
// REFRACT_WORLD (option bit 46): the texture's rgb times the picture behind
// the pixel as post-processing left it (soft_raster.h's RefractsWorld); set
// only where the backend has that (an overlay draw, after the resolve)
static const uint kShadeRefract = 1048576u;
// ENABLE_AO with a point light: the point lights' occlusion is the vertex
// colour's directional (SH) visibility toward each, AoShVertex, per vertex
static const uint kShadeAoSh = 2097152u;

// Register names are the game shaders' (scene_capture.h's kShadeRegs), PS
// unless VS is said.
struct ShadeParams {
    uint4 flags;           // x the bits above, y point lights (0-2)
    float4 color;          // c0, the material colour
    float4 ambient;        // c1; (1,1,1,1) when the material isn't lit
    float4 specular;       // c2, rgb and power
    float4 emissive;       // c5: x glow multiplier, y intensify
    float4 bloom;          // c7, the luminance weights PSEUDO_HDR writes alpha with
    float4 eye;            // the camera's world position
    float4 ao;             // x: VS c24, the occlusion's strength (both kinds)
    float4 texgen[2];      // VS c20, c21: u' = dot(row0.xyw, (u, v, 1)), v' likewise
    float4 point_pos[2];   // c64, c65: position, 1/(falloff - range)
    float4 point_color[2]; // c67, c68: colour, range/(range - falloff)
    float4 box[6];         // c80..c85, faces +X -X +Y -Y +Z -Z
    float4 rim;            // c63, rgb and power
    float4 fade[3];        // c53, c54 (left and right planes), c55 (end, 1/(end - start), max)
    float4 fade_color;     // c104
    float4 alpha_cut;      // x: the threshold, 0-255, as RndMat keeps it
};
