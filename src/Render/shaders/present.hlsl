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
    float4 frame_size;  // w, h; z 1 when it matches rect
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
    if (frame_size.z != 0.0) return float4(frame_tex.Load(int3(int2(p), 0)).rgb, 1.0);
    return float4(frame_tex.SampleLevel(frame_sampler, p / rect.zw, 0.0).rgb, 1.0);
}
