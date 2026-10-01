"""build_shaders: compiles the native view's shaders into a C header.

  python tools/shaders/build_shaders.py

Compiles src/Render/shaders/mesh.hlsl twice: DXBC (fxc, shader model 5.1) for
SDL_gpu's Direct3D 12 backend and SPIR-V (dxc) for its Vulkan backend, checks
the SPIR-V (spirv-val, and spirv-cross --reflect for the descriptor sets SDL_gpu
expects), and writes src/Render/shaders/mesh_shaders.gen.h with the bytes, so
building band3 needs none of these tools. Run it after changing mesh.hlsl and
check in the header.

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
SOURCE = os.path.join(SHADER_DIR, "mesh.hlsl")
HEADER = os.path.join(SHADER_DIR, "mesh_shaders.gen.h")

# (array name stem, entry point, fxc profile, dxc profile)
STAGES = [
    ("kMeshVertex", "VSMain", "vs_5_1", "vs_6_0"),
    ("kMeshPixel", "PSMain", "ps_5_1", "ps_6_0"),
]

# what spirv-cross --reflect must report, per stage: SDL_gpu's sets are 0 vertex
# resources, 1 vertex uniforms, 2 pixel resources, 3 pixel uniforms
EXPECTED_BINDINGS = {
    "VSMain": {("ubos", "VertexUniforms", 1, 0), ("ssbos", "bones", 0, 0)},
    "PSMain": {("ubos", "PixelUniforms", 3, 0), ("textures", "tex", 2, 0)},
}


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


def check_reflection(entry, reflect_json):
    data = json.loads(reflect_json)
    found = set()
    for kind in ("ubos", "ssbos", "textures", "separate_images", "separate_samplers"):
        for item in data.get(kind, []):
            # dxc names a cbuffer's block type.<name>
            name = item["name"].removeprefix("type.")
            found.add((kind, name, item.get("set"), item.get("binding")))
    expected = EXPECTED_BINDINGS[entry]
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

    arrays = []
    with tempfile.TemporaryDirectory() as tmp:
        for stem, entry, fxc_profile, dxc_profile in STAGES:
            dxbc = os.path.join(tmp, entry + ".dxbc")
            run([fxc, "/nologo", "/O3", "/Qstrip_debug", "/Qstrip_reflect",
                 "/T", fxc_profile, "/E", entry, "/Fo", dxbc, SOURCE])
            spirv = os.path.join(tmp, entry + ".spv")
            run([dxc, "-spirv", "-fspv-target-env=vulkan1.1", "-O3",
                 "-T", dxc_profile, "-E", entry, "-Fo", spirv, SOURCE])
            run([spirv_val, "--target-env", "vulkan1.1", spirv])
            check_reflection(entry, run([spirv_cross, spirv, "--reflect"]))
            with open(dxbc, "rb") as f:
                arrays.append(c_array(stem + "Dxbc", f.read()))
            with open(spirv, "rb") as f:
                arrays.append(c_array(stem + "Spirv", f.read()))
            print(f"{entry}: {os.path.getsize(dxbc)} bytes DXBC, "
                  f"{os.path.getsize(spirv)} bytes SPIR-V")

    header = "\n".join([
        "// Generated by tools/shaders/build_shaders.py from mesh.hlsl; don't edit.",
        "// DXBC (shader model 5.1) for SDL_gpu's Direct3D 12 backend, SPIR-V for its",
        "// Vulkan backend. Entry points VSMain and PSMain.",
        "#pragma once",
        "",
        "namespace band3::render::shaders {",
        "",
        "\n\n".join(arrays),
        "",
        "}  // namespace band3::render::shaders",
        "",
    ])
    with open(HEADER, "w", newline="\n") as f:
        f.write(header)
    print(f"wrote {os.path.relpath(HEADER, REPO)}")


if __name__ == "__main__":
    main()
