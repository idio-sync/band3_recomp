// A spotlight cone's inputs: the PS constants NgSpotlightDrawer::RenderConeDefs
// sets (spotlight_survey.md 3), packed by spot_model.cpp. Included as C++ by
// spot_model.h. Only float4, so HLSL's cbuffer packing matches the C++ layout.

struct SpotParams {
    float4 eye;          // c10
    float4 apex;         // c25: the cone's apex (xyz), 1 / its length (w)
    float4 axis;         // c26: its axis, unit (xyz), its length (w)
    float4 eye_apex;     // c27: eye - apex (xyz)
    float4 cone;         // c28: w the cosine of its half angle, squared
    float4 forward;      // c30: the camera's forward axis (xyz), -forward.eye (w)
    float4 xsec;         // c86: x the cross-section texture's weight (0 or 1)
    float4 plane_a;      // c87, c88: the cross-section's planes (normal xyz, offset w)
    float4 plane_b;
    float4 depth_range;  // c89: the camera's near, far, 1/(zmax-zmin), zmin/(zmax-zmin)
    float4 color;        // c90: colour * intensity * 8 (rgb)
    float4 fog;          // c127: (0, 1/far, 0, 0) while the cones draw
    float4 target;       // the depth volume's width, height, 1/width, 1/height
};
