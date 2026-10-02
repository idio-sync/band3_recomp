"""build_shaders: compiles the native view's shaders into a C header.

  python tools/shaders/build_shaders.py [present.hlsl ...]

Compiles src/Render/shaders/mesh.hlsl (and the shade*.hlsli and spot*.hlsli it
includes), post.hlsl (and the post*.hlsli it includes) and gamma.hlsl twice
each: DXBC (fxc, shader model 5.1) for SDL_gpu's Direct3D 12 backend and SPIR-V
(dxc) for its Vulkan backend, checks the SPIR-V (spirv-val, and spirv-cross
--reflect for the descriptor sets SDL_gpu expects), and writes
src/Render/shaders/mesh_shaders.gen.h, post_shaders.gen.h and
gamma_shaders.gen.h with the bytes, so building band3 needs none of these tools.
present.hlsl, the native renderer's picture drawn on the SDK presenter's own
Direct3D 12 command list, is DXBC alone, into present_shaders.gen.h.
Run it after changing a shader and check in the headers; name shader files to
compile those alone and leave the other headers as they are.

Finds fxc in the Windows 10 SDK and dxc, spirv-val and spirv-cross in the Vulkan
SDK (VULKAN_SDK, or C:/VulkanSDK/<newest>); FXC, DXC, SPIRV_VAL and SPIRV_CROSS
override them. Standard library only.
"""

import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHADER_DIR = os.path.join(REPO, "src", "Render", "shaders")
# Each shader file: its header, and its stages as (array name stem, entry
# point, fxc profile, dxc profile, what spirv-cross --reflect must report).
# SDL_gpu's sets are 0 vertex resources, 1 vertex uniforms, 2 pixel resources,
# 3 pixel uniforms.
SHADERS = [
    ("mesh.hlsl", "mesh_shaders.gen.h", [
        ("kMeshVertex", "VSMain", "vs_5_1", "vs_6_0",
         {("ubos", "VertexUniforms", 1, 0), ("ssbos", "bones", 0, 0)}),
        ("kMeshPixel", "PSMain", "ps_5_1", "ps_6_0",
         {("ubos", "PixelUniforms", 3, 0), ("textures", "tex", 2, 0),
          ("textures", "spec_tex", 2, 1), ("textures", "glow_tex", 2, 2),
          ("textures", "proj_tex", 2, 3), ("textures", "gobo_tex", 2, 4),
          ("textures", "behind_tex", 2, 5), ("textures", "shadow_tex", 2, 6),
          ("textures", "normal_tex", 2, 7), ("textures", "detail_tex", 2, 8)}),
        ("kSpotPixel", "PSSpotCone", "ps_5_1", "ps_6_0",
         {("ubos", "PixelUniforms", 3, 0), ("ubos", "SpotUniforms", 3, 1),
          ("textures", "tex", 2, 0), ("textures", "scene_depth_tex", 2, 9),
          ("textures", "density_tex", 2, 10)}),
        ("kSoftPixel", "PSSoftParticle", "ps_5_1", "ps_6_0",
         {("ubos", "PixelUniforms", 3, 0), ("ubos", "SpotUniforms", 3, 1),
          ("textures", "tex", 2, 0), ("textures", "spec_tex", 2, 1),
          ("textures", "glow_tex", 2, 2), ("textures", "proj_tex", 2, 3),
          ("textures", "gobo_tex", 2, 4), ("textures", "behind_tex", 2, 5),
          ("textures", "shadow_tex", 2, 6), ("textures", "normal_tex", 2, 7),
          ("textures", "detail_tex", 2, 8), ("textures", "scene_depth_tex", 2, 9)}),
        # a shadow map's depth: no resources
        ("kShadowDepthPixel", "PSShadowDepth", "ps_5_1", "ps_6_0", set()),
    ]),
    ("post.hlsl", "post_shaders.gen.h", [
        ("kFullscreenVertex", "VSFullscreen", "vs_5_1", "vs_6_0", set()),
        ("kResolvePixel", "PSResolve", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0),
          ("textures", "depth_tex", 2, 1)}),
        ("kDownsamplePixel", "PSDownsample", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0)}),
        ("kBlurPixel", "PSBlur", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0)}),
        ("kGlarePixel", "PSGlare", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0)}),
        ("kCompositePixel", "PSComposite", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0),
          ("textures", "depth_tex", 2, 1), ("textures", "dof_tex", 2, 2),
          ("textures", "bloom0_tex", 2, 3), ("textures", "bloom1_tex", 2, 4),
          ("textures", "bloom2_tex", 2, 5), ("textures", "volume_tex", 2, 6),
          ("textures", "density_tex", 2, 7), ("textures", "soft_tex", 2, 8),
          ("textures", "noise_tex", 2, 9)}),
        ("kCompositeHistoryPixel", "PSCompositeHistory", "ps_5_1", "ps_6_0",
         {("ubos", "PostUniforms", 3, 0), ("textures", "color_tex", 2, 0),
          ("textures", "depth_tex", 2, 1), ("textures", "dof_tex", 2, 2),
          ("textures", "bloom0_tex", 2, 3), ("textures", "bloom1_tex", 2, 4),
          ("textures", "bloom2_tex", 2, 5), ("textures", "volume_tex", 2, 6),
          ("textures", "density_tex", 2, 7), ("textures", "soft_tex", 2, 8),
          ("textures", "noise_tex", 2, 9), ("textures", "prev_tex", 2, 10)}),
    ]),
    # the display gamma ramp's pass, drawn with post.hlsl's VSFullscreen
    ("gamma.hlsl", "gamma_shaders.gen.h", [
        ("kGammaPixel", "PSGamma", "ps_5_1", "ps_6_0",
         {("ubos", "GammaUniforms", 3, 0), ("textures", "color_tex", 2, 0)}),
    ]),
    # the native renderer's picture on the window, on the SDK presenter's
    # Direct3D 12 command list: DXBC alone (no dxc profile), with the root
    # signature native_view.cpp makes rather than SDL_gpu's layout
    ("present.hlsl", "present_shaders.gen.h", [
        ("kPresentVertex", "VSPresent", "vs_5_1", None, None),
        ("kPresentPixel", "PSPresent", "ps_5_1", None, None),
    ]),
]


def find_tool(env, names, patterns):
    if os.environ.get(env):
        return os.environ[env]
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for pattern in patterns:
        # the newest SDK first
        matches = sorted(glob.glob(pattern), reverse=True)
        if matches:
            return matches[0]
    sys.exit(f"build_shaders: can't find {names[0]}; set {env}")


def vulkan_sdk_bins():
    bins = []
    if os.environ.get("VULKAN_SDK"):
        bins.append(os.path.join(os.environ["VULKAN_SDK"], "Bin"))
        bins.append(os.path.join(os.environ["VULKAN_SDK"], "bin"))
    bins += sorted(glob.glob("C:/VulkanSDK/*/Bin"), reverse=True)
    return bins


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"build_shaders: {' '.join(command)} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def check_reflection(entry, expected, reflect_json):
    data = json.loads(reflect_json)
    found = set()
    for kind in ("ubos", "ssbos", "textures", "separate_images", "separate_samplers"):
        for item in data.get(kind, []):
            # dxc names a cbuffer's block type.<name>
            name = item["name"].removeprefix("type.")
            found.add((kind, name, item.get("set"), item.get("binding")))
    if found != expected:
        sys.exit(f"build_shaders: {entry}'s SPIR-V bindings are {sorted(found)}, "
                 f"SDL_gpu wants {sorted(expected)}")


def c_array(name, data):
    lines = [f"inline constexpr unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    exe = ".exe" if os.name == "nt" else ""
    fxc = find_tool("FXC", ["fxc"], [
        "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe"])
    sdk = vulkan_sdk_bins()
    dxc = find_tool("DXC", ["dxc"], [os.path.join(b, "dxc" + exe) for b in sdk])
    spirv_val = find_tool("SPIRV_VAL", ["spirv-val"],
                          [os.path.join(b, "spirv-val" + exe) for b in sdk])
    spirv_cross = find_tool("SPIRV_CROSS", ["spirv-cross"],
                            [os.path.join(b, "spirv-cross" + exe) for b in sdk])

    only = set(sys.argv[1:])
    unknown = only - {source_name for source_name, _, _ in SHADERS}
    if unknown:
        sys.exit(f"build_shaders: no shader file {', '.join(sorted(unknown))}")
    for source_name, header_name, stages in SHADERS:
        if only and source_name not in only:
            continue
        source = os.path.join(SHADER_DIR, source_name)
        header_path = os.path.join(SHADER_DIR, header_name)
        arrays = []
        with tempfile.TemporaryDirectory() as tmp:
            for stem, entry, fxc_profile, dxc_profile, expected in stages:
                dxbc = os.path.join(tmp, entry + ".dxbc")
                run([fxc, "/nologo", "/O3", "/Qstrip_debug", "/Qstrip_reflect",
                     "/T", fxc_profile, "/E", entry, "/Fo", dxbc, source])
                with open(dxbc, "rb") as f:
                    arrays.append(c_array(stem + "Dxbc", f.read()))
                if dxc_profile is None:
                    print(f"{entry}: {os.path.getsize(dxbc)} bytes DXBC")
                    continue
                spirv = os.path.join(tmp, entry + ".spv")
                run([dxc, "-spirv", "-fspv-target-env=vulkan1.1", "-O3",
                     "-T", dxc_profile, "-E", entry, "-Fo", spirv, source])
                run([spirv_val, "--target-env", "vulkan1.1", spirv])
                check_reflection(entry, expected, run([spirv_cross, spirv, "--reflect"]))
                with open(spirv, "rb") as f:
                    arrays.append(c_array(stem + "Spirv", f.read()))
                print(f"{entry}: {os.path.getsize(dxbc)} bytes DXBC, "
                      f"{os.path.getsize(spirv)} bytes SPIR-V")

        entries = " and ".join(stage[1] for stage in stages)
        if all(stage[3] is None for stage in stages):
            what = ["// DXBC (shader model 5.1) for the SDK presenter's Direct3D 12 command list.",
                    f"// Entry points {entries}."]
        else:
            what = ["// DXBC (shader model 5.1) for SDL_gpu's Direct3D 12 backend, SPIR-V for its",
                    f"// Vulkan backend. Entry points {entries}."]
        header = "\n".join([
            f"// Generated by tools/shaders/build_shaders.py from {source_name}; don't edit.",
            *what,
            "#pragma once",
            "",
            "namespace band3::render::shaders {",
            "",
            "\n\n".join(arrays),
            "",
            "}  // namespace band3::render::shaders",
            "",
        ])
        with open(header_path, "w", newline="\n") as f:
            f.write(header)
        print(f"wrote {os.path.relpath(header_path, REPO)}")


if __name__ == "__main__":
    main()
