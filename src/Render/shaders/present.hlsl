// Experimental: the native renderer's picture on the game's window
// (the native picture shown, native_view.cpp's NativePresentDrawer). A triangle over
// the whole of the SDK presenter's back buffer, on its own Direct3D 12 command
// list, under the SDK's ImGui: black outside the picture's rectangle (the
// letterbox), and inside it gpu_view's finished frame, texel for texel when
// the frame is the rectangle's size and scaled bilinearly while it isn't yet
// (the window was just resized).
//
// Direct3D 12 only, with the root signature native_view.cpp makes: 8 root
// constants at b0, the frame at t0 and a linear clamping static sampler at s0.
// Elsewhere the drawer draws the picture through the SDK's immediate drawer.
//
// tools/shaders/build_shaders.py compiles it into present_shaders.gen.h; run
// it after changing this file.

cbuffer PresentConstants : register(b0) {
    // the picture's rectangle in the back buffer, in pixels: x, y, w, h
    float4 rect;
    // the frame's width and height; z 1 when it is the rectangle's size
    float4 frame_size;
};

Texture2D<float4> frame_tex : register(t0);
SamplerState frame_sampler : register(s0);

struct PresentIn {
    float4 pos : SV_Position;
};

// vertex 0, 1, 2: a triangle whose middle covers clip space
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
