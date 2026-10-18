// The native picture onto the SDK presenter's back buffer, under ImGui
// (native_view.cpp's NativePresentDrawer): letterboxed black, texel for texel
// at the rectangle's size, bilinear while a resize catches up.
//
// Direct3D 12 only, with native_view.cpp's root signature: 8 root constants
// at b0, the frame at t0, a linear clamping static sampler at s0. Elsewhere
// the SDK's immediate drawer draws the picture.
//
// tools/shaders/build_shaders.py compiles it into present_shaders.gen.h;
// rerun it after changing this file.

cbuffer PresentConstants : register(b0) {
    float4 rect;        // in back buffer pixels: x, y, w, h
    // w, h; z 1 when it matches rect; w how far the colour is faded to black
    // (VR's eyes at a cut, xr_present.cpp; 0 elsewhere)
    float4 frame_size;
};

Texture2D<float4> frame_tex : register(t0);
SamplerState frame_sampler : register(s0);

struct PresentIn {
    float4 pos : SV_Position;
};

PresentIn VSPresent(uint id : SV_VertexID) {
    PresentIn o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSPresent(PresentIn i) : SV_Target0 {
    const float2 p = i.pos.xy - rect.xy;
    if (any(p < 0.0) || any(p >= rect.zw)) return float4(0.0, 0.0, 0.0, 1.0);
    const float keep = 1.0 - frame_size.w;
    if (frame_size.z != 0.0) return float4(frame_tex.Load(int3(int2(p), 0)).rgb * keep, 1.0);
    return float4(frame_tex.SampleLevel(frame_sampler, p / rect.zw, 0.0).rgb * keep, 1.0);
}

// VR's eyes into a typed sRGB swapchain image (xr_present.cpp), which encodes
// what it's given: the picture's bytes are display values already, so they
// go in decoded, for the target to give them back
float SrgbToLinear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float4 PSPresentLinear(PresentIn i) : SV_Target0 {
    const float3 c = PSPresent(i).rgb;
    return float4(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b), 1.0);
}

// VR's HUD panel into its quad layer's image (xr_present.cpp): as PSPresent
// but its alpha kept, the coverage its colour is premultiplied by
// (gpu_view.h's HUD panel), and clear outside the rectangle
float4 PSPresentHud(PresentIn i) : SV_Target0 {
    const float2 p = i.pos.xy - rect.xy;
    if (any(p < 0.0) || any(p >= rect.zw)) return float4(0.0, 0.0, 0.0, 0.0);
    if (frame_size.z != 0.0) return frame_tex.Load(int3(int2(p), 0));
    return frame_tex.SampleLevel(frame_sampler, p / rect.zw, 0.0);
}

// Into an image the compositor reads as sRGB (typed so, or typeless in an
// sRGB swapchain), through an sRGB view that encodes what it's given; the
// compositor decodes and blends in linear light. Decoding premultiplied
// display values as they are (bytes verbatim) would darken
// every partly covered pixel (decode(c a) is less than decode(c) a), so the
// covered part goes in unpremultiplied, decoded, premultiplied again, and
// colour beyond alpha (additive light, which premultiplied colour holds only
// by adding: alpha 0 with colour) decoded by itself and added. Covered and
// clear pixels come back as their bytes, as the eyes' do.
float3 PremultipliedToLinear(float3 c, float a) {
    const float3 covered = min(c, a);
    const float3 added = c - covered;
    float3 lin = float3(SrgbToLinear(added.r), SrgbToLinear(added.g), SrgbToLinear(added.b));
    if (a > 0.0) {
        const float3 s = covered / a;
        lin += float3(SrgbToLinear(s.r), SrgbToLinear(s.g), SrgbToLinear(s.b)) * a;
    }
    return lin;
}

float4 PSPresentHudLinear(PresentIn i) : SV_Target0 {
    const float4 c = PSPresentHud(i);
    return float4(PremultipliedToLinear(c.rgb, c.a), c.a);
}
