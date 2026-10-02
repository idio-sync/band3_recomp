# Shader research scripts

Python models of Rock Band 3's own shaders, checked against the shaders' Xenos
microcode, that the native view's shading (`src/Render/shaders/shade.hlsli`) and
`tests/shade_model_test.cpp` are built on. Standard library only.

| Script | |
|---|---|
| `xsim.py` | runs a dumped shader's microcode disassembly (`.ucode.frag`/`.vert`) on given constants, registers and texture samples, so a model can be checked against what the shader really computes |
| `hyp.py` | helpers the models share: random setups for xsim, the box and point lights, reflection, Fresnel |
| `fam.py` | first pass at the standard material, and `refs()`, which lists the constants and samplers a shader reads |
| `fam3.py` | the standard material's model (normal and detail maps, AO, prelit, shadow, projected light, environment, specular, rim, glow); `python fam3.py <hash>...` searches for the options each shader matches |
| `skin2.py` | the skin family's model (wrap diffuse, two normals, squared Fresnel); `python skin2.py <hash>...` |
| `hair3.py` | the hair family's model (wrap diffuse, two-colour strand highlight); `python hair3.py <hash>...` |
| `vs.py` | setup helpers for running vertex shaders in xsim |
| `crowd.py` | the crowd's billboards (BILLBOARD, option bit 25): their vertex shaders' turn to the camera and light, and their pixel shader; `python crowd.py` checks the model against the microcode |
| `movie.py` | the movie's pixel shader (ShaderType 11): a Bink frame's Y, cR and cB planes to RGB, which `shade.hlsli`'s `MovieRgb` is; `python movie.py` checks the model against the microcode, `--cases` prints the cases `tests/shade_model_test.cpp` checks |
| `lit.py`, `vlit.py` | find each dumped shader's literal constants (c240-c255, which the disassembly leaves out) in the game's shader blobs and write `lits.json` |
| `gen_shade_cases.py` | prints `kCases` in `tests/shade_model_test.cpp` from the three models |
| `post/check_post.py` | checks models of the post-processing pixel shaders (DOF, bloom, glare, the spotlights' term, bright pass, downsample, blur kernels, glare's pass over bloom's level 0) against their microcode, sample positions included; exits 1 on a mismatch |
| `post/neg_controls.py` | deliberately wrong post models, which `check_post.py` must fail |
| `post/check_noise.py` | checks the composite's noise (film grain) term, `post_model.hlsli`'s `NoiseTerm`, against the three straight-line composite variants that have it (glare, DOF, bloom and the spotlights' term around it), taps included; exits 1 on a mismatch |
| `post/neg_noise.py` | deliberately wrong noise models (overlay per channel, no 6.75, arithmetic mean), which `check_noise.py` must fail |

## Inputs

Everything the scripts read comes from your own copy of the game, and none of it
is in the repo: not the shader dump, the blobs extracted from the ark or
`lits.json`. They default to places under `out/`:

| | Default | Override |
|---|---|---|
| the `--dump_shaders` output | `out/shaders` | `BAND3_SHADER_DUMP` |
| the ark's shader blobs | `out/shader_research` | `BAND3_SHADER_WORK` |
| `lits.json` | `out/shader_research/lits.json` | `BAND3_SHADER_LITS` |

1. Dump the shaders the game uses. The dump path must be absolute:

   ```
   python tools/band3ctl.py launch --fresh -- --dump_shaders=<abs path>/out/shaders
   ```

   then play a song (for instance `tests/game/boot.b3t` followed by
   `tests/game/render_song.b3t`, see their headers) so its shaders get built, and
   `python tools/band3ctl.py quit`.
2. Extract two entries of `assets/gen/main_xbox_0.ark` into `out/shader_research`:
   `xbox_shaders` (offset 0, 14619013 bytes) as `xbox_shaders.bin` and
   `xbox_preinit_shaders` (offset 14978726, 137549 bytes) as
   `xbox_preinit_shaders.bin`.
3. `python tools/shaders/research/lit.py`, then `python tools/shaders/research/vlit.py`,
   which make `lits.json`.

## Use

```
python tools/shaders/research/gen_shade_cases.py      # kCases, as in tests/shade_model_test.cpp
python tools/shaders/research/post/check_post.py [trials]
python tools/shaders/research/fam3.py <shader hash>...
```

`gen_shade_cases.py` runs only the models, so it needs none of the inputs above;
its output is `kCases`' entries exactly. The others run the game's microcode, so
they need the dump and `lits.json`.
