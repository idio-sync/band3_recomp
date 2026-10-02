"""The crowd's billboards (ShaderType 12 with BILLBOARD, option bit 25), modelled
and checked against the game's microcode.

WorldCrowd draws each crowd character type's impostor into a texture, then
every instance of that type as one quad of a DxMultiMesh (the four CPU
vertices WorldCrowd rewrites, in the quad's local XZ). Its vertex shaders
(4B19F15CA3B46FEB lit: REAL_LIGHTS, APPROX_LIGHTS, one point light; and
7D050DB197258C07 unlit) turn the quad to the camera:

    P = T + x R + y F + z U,  N = n.x R + n.y F + n.z U

with R, U and F the camera's right, up and forward, the columns of VS
c16..c18 (the inverse view: c16 = (R.x, U.x, F.x, eye.x) and so on), and T
the instance's translation (c92..c94.w, indexed by the instance). The
instance's rotation and scale (c92..c94.xyz) are left out. The lit one's
colour is vertex light with no N.L on the point light and no AO, whatever
the option word says:

    colour.rgb = c0 (c1 + att c67 + box(N / |N|)),  att = sat(|c64 - P| c64.w + c67.w)
    colour.a   = c0.a c1.a

and the unlit one's c0 c1. The pixel shader is the vertex-lit family's
(729384CD01836AE2; 34B29856453FE506 without the added colour): the texture
times that colour, alpha too, plus o3, which the billboard VS writes 0:

    rgb = tex.rgb colour.rgb, a = tex.a colour.a

`python crowd.py` runs each shader on random inputs against the model and
exits 1 on a mismatch.
"""
import math, random, re, sys
import xsim
from hyp import rv, dot, sat, box, close

VS_LIT = '4B19F15CA3B46FEB'
VS_UNLIT = '7D050DB197258C07'
PS = ('729384CD01836AE2', '34B29856453FE506')


def camera_axes(c):
    """R, U, F from VS c16..c18's columns"""
    return ([c[16][0], c[17][0], c[18][0]], [c[16][1], c[17][1], c[18][1]],
            [c[16][2], c[17][2], c[18][2]])


def billboard(c, v, w=1.0, inst=0):
    """the billboard's world position (w 1) or normal (w 0) of local v"""
    R, U, F = camera_axes(c)
    t = [c[92 + 3 * inst + i][3] * w for i in range(3)]
    return [t[k] + v[0] * R[k] + v[1] * F[k] + v[2] * U[k] for k in range(3)]


def colour_lit(c, P, N):
    l = math.sqrt(dot(N, N))
    n = [x / l for x in N]
    L = [c[64][k] - P[k] for k in range(3)]
    att = sat(math.sqrt(dot(L, L)) * c[64][3] + c[67][3])
    b = box(n, c)
    rgb = [c[0][k] * (c[1][k] + att * c[67][k] + b[k]) for k in range(3)]
    return rgb + [c[0][3] * c[1][3]]


def run_vs(h, seed, inst):
    random.seed(seed)
    c = {i: rv(-1, 1) for i in range(256)}
    c.update(xsim.lits(h))
    c[89] = [4.0, 0, 0, 0]  # vertices per instance
    for k in (64, 67):
        c[k][3] = random.uniform(-0.01, 0.0) if k == 64 else random.uniform(0.5, 2.0)
    c[64] = [random.uniform(-3, 3) for _ in range(3)] + [c[64][3]]
    pos, nrm, uv = rv(-2, 2), rv(-1, 1), rv(0, 1)

    def fetch(op, unit, coord, line):
        if 'vf1' in line:
            return [coord[0]] * 4  # the index buffer: the vertex itself
        m = re.search(r'Offset=(\d+)', line)
        off = int(m.group(1)) if m else 0
        return {0: pos, 4: nrm, 16: uv}[off]

    # the vertex's index: instance inst's first, so a0 = 3 inst
    st = xsim.State(c, {0: [4.0 * inst, 0, 0, 0]}, fetch)
    xsim.run(xsim.load(h, 'vert'), st)
    return st, c, pos, nrm, uv


def check_vs(h, trials):
    bad = 0
    for seed in range(trials):
        inst = seed % 3
        st, c, pos, nrm, uv = run_vs(h, seed, inst)
        P = billboard(c, pos, 1.0, inst)
        clip = [sum(c[4 + r][k] * (P + [1.0])[k] for k in range(4)) for r in range(4)]
        got = st.o['oPos']
        ok = close(got, clip) and close(st.o['o1'][:3], P)
        u = [dot(uv[:2] + [0], c[20][:2] + [0]) + c[20][3], dot(uv[:2] + [0], c[21][:2] + [0]) + c[21][3]]
        ok &= close(st.o['o0'][:2], u)
        if h == VS_LIT:
            ok &= close(st.o['o2'], colour_lit(c, P, billboard(c, nrm, 0.0, inst)))
            ok &= close(st.o['o3'][:3], [0, 0, 0])
        else:
            ok &= close(st.o['o2'], [c[0][k] * c[1][k] for k in range(4)])
        if not ok:
            bad += 1
            print('MISMATCH', h, seed, got, clip, st.o)
    print(h, 'vs', trials - bad, '/', trials)
    return bad


def check_ps(h, trials):
    bad = 0
    for seed in range(trials):
        random.seed(seed)
        tex = rv(0, 1)
        r = {0: rv(0, 1), 1: rv(), 2: rv(0, 2), 3: rv(0, 1)}
        st = xsim.State({}, r, lambda *a: tex)
        col, added = list(r[2]), list(r[3])
        xsim.run(xsim.load(h, 'frag'), st)
        want = [tex[k] * col[k] + (added[k] if h == PS[0] else 0) for k in range(3)]
        want.append(tex[3] * col[3])
        if not close(st.o['oC0'], want):
            bad += 1
            print('MISMATCH', h, seed, st.o['oC0'], want)
    print(h, 'ps', trials - bad, '/', trials)
    return bad


if __name__ == '__main__':
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    bad = check_vs(VS_LIT, n) + check_vs(VS_UNLIT, n) + sum(check_ps(h, n) for h in PS)
    sys.exit(1 if bad else 0)
