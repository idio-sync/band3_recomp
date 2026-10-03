"""REFRACT_WORLD's pixel shader (option bit 46; the score box's glass),
FC53125B5EB914F8: the diffuse texture (tf0) times the picture behind the
pixel (tf6, DxRnd::GetCurrentFrameTex), read where a second map (tf1, the
material's refract normal map, NgMat::SetupShader) nudges it:

    n  = tf1(uv).xy
    s  = (clip.x + k (2 n.y - 1), clip.y + k (2 n.x - 1)) / clip.w * (0.5, -0.5) + 0.5
    rgb = tf0.rgb * tf6(s).rgb * col.rgb
    a   = tf0.a * col.a

uv is the texture's (interpolator 0, as tf0 reads it), clip the vertex
shader's position (interpolator 2: its unlit VS 8CCB788DA4EB870B copies oPos
there), col interpolator 3 (that VS's c1 * c0), k PS c119.w (the material's
refract strength, all four components; 2.1 on the score box). Note n's yx:
x moves with the map's green, y with its red. The shader's literals, c255 =
(0.5, -0.5, -1, 2), come from lits.json. shade.hlsli's RefractUv is the
s line.

`python refract.py [trials]` runs the microcode (xsim) on random inputs and
checks the model; exits 1 on a mismatch. `python refract.py --cases` prints
the cases tests/shade_model_test.cpp checks RefractUv against.
"""
import random, sys

HASH = 'FC53125B5EB914F8'
C255 = (0.5, -0.5, -1.0, 2.0)


def refract_uv(clip, k, n):
    return [((clip[0] + k * (2 * n[1] - 1)) / clip[3]) * 0.5 + 0.5,
            ((clip[1] + k * (2 * n[0] - 1)) / clip[3]) * -0.5 + 0.5]


def check(trials):
    import xsim
    lits = xsim.lits(HASH)
    if tuple(lits.get(255, ())) != C255:
        print('literal c255 is %s, the model has %s' % (lits.get(255), C255))
        return False
    prog = xsim.load(HASH)
    ok = True
    for t in range(trials):
        random.seed(t)
        uv = [random.uniform(-1, 2), random.uniform(-1, 2)]
        clip = [random.uniform(-50, 50), random.uniform(-50, 50), random.uniform(0, 50),
                random.uniform(1, 60)]
        col = [random.random() for _ in range(4)]
        k = random.uniform(0, 4)
        n = [random.random() for _ in range(4)]
        diffuse = [random.random() for _ in range(4)]
        seen = {}

        def fetch(op, unit, coord, line):
            seen[unit] = coord
            if unit == 'tf1':
                return n
            if unit == 'tf0':
                return diffuse
            # the picture behind: a texel that says where it was read
            return [coord[0], coord[1], 0.25, 0.75]

        consts = {r: list(v) for r, v in lits.items()}
        consts[119] = [k, k, k, k]
        st = xsim.State(consts,
                        {0: uv + [0.0, 0.0], 2: clip, 3: col}, fetch)
        xsim.run(prog, st)
        got = st.o['oC0']
        s = refract_uv(clip, k, n)
        want = [diffuse[0] * s[0] * col[0], diffuse[1] * s[1] * col[1],
                diffuse[2] * 0.25 * col[2], diffuse[3] * col[3]]
        reads_uv = all(abs(a - b) < 1e-9 for a, b in zip(seen['tf0'][:2], uv)) and \
            all(abs(a - b) < 1e-9 for a, b in zip(seen['tf1'][:2], uv))
        if not reads_uv or any(abs(g - w) > 1e-6 for g, w in zip(got, want)):
            print('trial %d: microcode %s, model %s (tf0 at %s, tf1 at %s)' %
                  (t, got, want, seen.get('tf0'), seen.get('tf1')))
            ok = False
    print('%s: %d trials, %s' % (HASH, trials, 'match' if ok else 'MISMATCH'))
    return ok


def cases():
    # the score box's strength, a neutral texel, the extremes, and an unbound
    # map (0: the most to the left and down)
    for clip, k, n in (((10, -20, 5, 100), 2.1, (128 / 255, 128 / 255)),
                       ((10, -20, 5, 100), 2.1, (1, 0)),
                       ((-35, 12, 5, 80), 2.1, (0.25, 0.9)),
                       ((0, 0, 1, 1), 1.0, (0, 0)),
                       ((3, 4, 1, 2), 0.5, (0.75, 0.2))):
        s = refract_uv(clip, k, n)
        print('    {{%gf, %gf, %gf, %gf}, %gf, {%.9gf, %.9gf}, {%.9gf, %.9gf}},' %
              (*clip, k, *n, *s))


if __name__ == '__main__':
    if '--cases' in sys.argv:
        cases()
    else:
        sys.exit(0 if check(int(sys.argv[1]) if len(sys.argv) > 1 else 50) else 1)
