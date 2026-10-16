# OpenXR headers

`include/openxr/openxr.h`, `openxr_platform.h` and `openxr_platform_defines.h`, unchanged,
from the Khronos Group's [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) at tag
`release-1.1.63` (OpenXR 1.1.63, 2026-09-02). They're licensed `Apache-2.0 OR MIT`; band3
takes them under the MIT licence (`LICENSE.txt`), which goes with its GPL-2.

Only the headers are here. band3 never links the OpenXR loader: `src/Render/xr_present.cpp`
loads `openxr_loader.dll` (Apache-2.0, shipped by the OpenXR runtime, SteamVR's among them)
at run time, when the `vr` setting is on, and goes on without VR if it can't.
