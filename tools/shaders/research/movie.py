"""The movie's pixel shader (ShaderType 11, kMovieShader; Movie::Impl::Draw),
22F426E8D3A1F1B5: a Bink frame's Y (tf0), cR (tf2) and cB (tf3) planes to RGB.

    R = a Y + d cR + r0
    G = a Y + e cR + f cB + g0
    B = a Y + h cB + b0
    A = 1

with the shader's literals (c254, c255; lit.py), which are Bink's BT.601
matrix with its offsets folded in: a = 1.164124 (255/219), d = 1.595795,
e = -0.813477, f = -0.391449, h = 2.017822. No saturate (the target clamps),
no gamma, and neither the material's colour nor the vertex colour.

`python movie.py [trials]` runs the microcode (xsim) on random planes and
checks the model; exits 1 on a mismatch. `python movie.py --cases` prints the
cases tests/shade_model_test.cpp checks MovieRgb against (no inputs needed).
"""
import random, sys

HASH = '22F426E8D3A1F1B5'
# c254 and c255 as lit.py reads them from the shader's literal block
C254 = (1.16412353515625, -0.8134765625, 1.595794677734375, -0.8706550598144531)
C255 = (-0.391448974609375, 2.017822265625, 0.5297050476074219, -1.0816688537597656)


def model(y, cr, cb):
    a = C254[0]
    return [a * y + C254[2] * cr + C254[3],
            a * y + C254[1] * cr + C255[0] * cb + C255[2],
            a * y + C255[1] * cb + C255[3],
            1.0]


def check(trials):
    import xsim
    lits = xsim.lits(HASH)
    for r, want in ((254, C254), (255, C255)):
        if tuple(lits[r]) != want:
            print('literal c%d is %s, the model has %s' % (r, lits[r], want))
            return False
    prog = xsim.load(HASH)
    ok = True
    for t in range(trials):
        random.seed(t)
        planes = {'tf0': random.random(), 'tf2': random.random(), 'tf3': random.random()}
        # each plane's texel: its value in x, junk elsewhere (the shader reads x)
        tex = {u: [v, random.random(), random.random(), random.random()]
               for u, v in planes.items()}
        st = xsim.State({k: list(v) for k, v in lits.items()},
                        {i: [random.uniform(-1, 1) for _ in range(4)] for i in range(4)},
                        lambda op, unit, coord, line: tex[unit])
        xsim.run(prog, st)
        got = st.o['oC0']
        want = model(planes['tf0'], planes['tf2'], planes['tf3'])
        if any(abs(g - w) > 1e-6 for g, w in zip(got, want)):
            print('trial %d: microcode %s, model %s' % (t, got, want))
            ok = False
    print('%s: %d trials, %s' % (HASH, trials, 'match' if ok else 'MISMATCH'))
    return ok


def cases():
    # black, white, grey, and saturated colours, as 8-bit plane values
    for y, cr, cb in ((16, 128, 128), (235, 128, 128), (126, 128, 128), (82, 240, 90),
                      (145, 34, 54), (41, 110, 240), (200, 200, 30)):
        v = [x / 255 for x in (y, cr, cb)]
        r = model(*v)
        print('    {%d, %d, %d, {%.6ff, %.6ff, %.6ff, %.1ff}},' % (y, cr, cb, *r))


if __name__ == '__main__':
    if '--cases' in sys.argv:
        cases()
    else:
        sys.exit(0 if check(int(sys.argv[1]) if len(sys.argv) > 1 else 50) else 1)
