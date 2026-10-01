// Experimental: what a spotlight's cone shades with (spot_model.hlsli), packed
// by spot_model.cpp from the cone draw's shade state: the pixel shader
// constants NgSpotlightDrawer::RenderConeDefs set (out/research/
// spotlight_survey.md 3). Shared by mesh.hlsl and the CPU (spot_model.h
// includes it as C++, with float4 its own), as shade_params.hlsli is for
// shading. Everything is a float4: HLSL's cbuffer packing and C++'s layout
// are then the same.

struct SpotParams {
    float4 eye;          // c10: the camera's position (xyz)
    float4 apex;         // c25: the cone's apex (xyz), 1 / its length (w)
    float4 axis;         // c26: its axis, unit (xyz), its length (w)
    float4 eye_apex;     // c27: eye - apex (xyz)
    float4 cone;         // c28: w the cosine of its half angle, squared
    float4 forward;      // c30: the camera's forward axis (xyz), -forward.eye (w)
    float4 xsec;         // c86: x how much the cross-section texture counts (0 or 1)
    float4 plane_a;      // c87, c88: the cross-section's planes (normal xyz, offset w)
    float4 plane_b;
    float4 depth_range;  // c89: the camera's near, far, 1/(zmax-zmin), zmin/(zmax-zmin)
    float4 color;        // c90: the spot's colour times its intensity times 8 (rgb)
    float4 fog;          // c127: (0, 1/far, 0, 0) while the cones draw
    float4 target;       // the depth volume: width, height, 1/width, 1/height
};
