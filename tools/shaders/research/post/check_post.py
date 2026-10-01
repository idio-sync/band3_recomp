"""Check RB3's post-processing pixel shaders (Xenia ucode dumps) against Python models.

Runs each shader in xsim (tools/shaders/research) with random constants,
random texture contents and a random interpolated uv in r0, and compares oC0 with a
model of the formula the M4 research read from its microcode. Also records
every fetch's (sampler, coordinate) and checks it against the model's tap list.
Reads the ucode dump and lits.json from xsim.DUMP and xsim.LITS.

Usage: python check_post.py [trials]   (default 30)
"""
import sys, os, math, random
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import xsim

TRIALS = int(sys.argv[1]) if len(sys.argv) > 1 else 30


def sat(x):
    return min(max(x, 0.0), 1.0)


class Tex:
    """Random but deterministic texture: a value per (sampler, coordinate)."""
    def __init__(self, rng):
        self.rng = rng
        self.cache = {}
        self.log = []

    def __call__(self, unit, u, v):
        key = (unit, round(u, 9), round(v, 9))
        if key not in self.cache:
            self.cache[key] = [self.rng.uniform(0, 1) for _ in range(4)]
        return self.cache[key]

    def fetch_cb(self, op, unit, coord, line):
        self.log.append((unit, coord[0], coord[1]))
        return list(self(unit, coord[0], coord[1]))


def run_shader(h, consts, uv, tex):
    c = {i: [0.0] * 4 for i in range(256)}
    c.update(consts)
    c.update(xsim.lits(h))          # literal c255 etc. win
    st = xsim.State(c, {0: [uv[0], uv[1], 0.0, 0.0]}, tex.fetch_cb)
    xsim.run(xsim.load(h), st)
    return st.o.get('oC0', [math.nan] * 4)


def rv(rng, lo=0.0, hi=1.0):
    return [rng.uniform(lo, hi) for _ in range(4)]


# ---------------------------------------------------------------- models
def dof_amount(c24, depth_x):
    t = (1.0 - depth_x) * c24[0] + c24[1]
    return sat(min(max(abs(t), c24[2]), c24[3]))


def m_composite_63306D35(c, uv, T):
    u, v = uv
    scene, blur, soft = T('tf6', u, v), T('tf8', u, v), T('tf4', u, v)
    L0, L1, L2 = T('tf7', u, v), T('tf11', u, v), T('tf15', u, v)
    a = dof_amount(c[24], T('tf9', u, v)[0])
    rgb = [scene[k] + (blur[k] - scene[k]) * a + soft[k] for k in range(3)]
    rgb = [1 - (1 - rgb[k]) * (1 - (L0[k] + L1[k] + L2[k]) * c[6][k]) for k in range(3)]
    out = [sat(sum(c[92 + ch][k] * rgb[k] for k in range(3)) + c[92 + ch][3]) for ch in range(3)]
    alpha = sat(scene[3] + (blur[3] - scene[3]) * a)
    taps = {(s, u, v) for s in ('tf4', 'tf6', 'tf7', 'tf8', 'tf9', 'tf11', 'tf15')}
    return out + [alpha], taps


def m_dof_only_C91275BB(c, uv, T):
    u, v = uv
    scene, blur = T('tf6', u, v), T('tf8', u, v)
    a = dof_amount(c[24], T('tf9', u, v)[0])
    out = [sat(scene[k] + (blur[k] - scene[k]) * a) for k in range(4)]   # no colour matrix
    return out, {(s, u, v) for s in ('tf6', 'tf8', 'tf9')}


def m_bloom_only_2F002AB2(c, uv, T):
    u, v = uv
    scene = T('tf6', u, v)
    L = [T(s, u, v) for s in ('tf7', 'tf11', 'tf15')]
    out = [sat(1 - (1 - scene[k]) * (1 - sum(l[k] for l in L) * c[6][k])) for k in range(3)]
    return out + [sat(scene[3])], {(s, u, v) for s in ('tf6', 'tf7', 'tf11', 'tf15')}


def m_glare_C6A009EA(c, uv, T):
    u, v = uv
    scene, L0 = T('tf6', u, v), T('tf7', u, v)
    out = [sat(scene[k] + 0.5 * L0[k] * c[6][k]) for k in range(3)]
    return out + [sat(scene[3])], {(s, u, v) for s in ('tf6', 'tf7')}


def quad_taps(c, uv):
    dx, dy = 2 * c[15][0], 2 * c[15][1]
    u, v = uv
    return [(u + sx * dx, v + sy * dy) for sx in (-1, 1) for sy in (-1, 1)]


def m_bright_F920AF5C(c, uv, T):
    taps = quad_taps(c, uv)
    out = [0.0] * 4
    for (u, v) in taps:
        t = T('tf7', u, v)
        for k in range(4):
            out[k] += 0.25 * t[k] * t[3]
    return out, {('tf7', u, v) for (u, v) in taps}


def m_down4_38448F55(c, uv, T):
    taps = quad_taps(c, uv)
    out = [0.0] * 4
    for (u, v) in taps:
        t = T('tf0', u, v)
        for k in range(4):
            out[k] += 0.25 * t[k]
    return out, {('tf0', u, v) for (u, v) in taps}


def m_kernel(n):
    def m(c, uv, T):
        out = [0.0] * 4
        taps = set()
        for i in range(n):
            u, v = uv[0] + c[31 + i][0], uv[1] + c[31 + i][1]
            t = T('tf0', u, v)
            taps.add(('tf0', u, v))
            for k in range(4):
                out[k] += t[k] * c[47 + i][k]          # all four weight components, per channel
        return out, taps
    return m


# ---------------------------------------------------------------- constants
def consts_composite(rng):
    c = {i: rv(rng, -1, 1) for i in range(256)}
    c[6] = rv(rng, 0, 2)
    # c24 as NgDOFProc::DoPost builds it: (1/(s-b), -s/(s-b), min(max,min), max<0?1:max), min/max in [0,1]
    s, b = rng.uniform(0, 1), rng.uniform(0, 1)
    if abs(s - b) < 1e-3:
        b = s + 0.01
    mx, mn = rng.uniform(0, 1), rng.uniform(0, 1)
    c[24] = [1 / (s - b), -s / (s - b), min(mx, mn), mx]
    return c


def consts_generic(rng):
    return {i: rv(rng, -1, 1) for i in range(256)}


CASES = [
    ('63306D35C02782FB', 'composite (DOF+soft+bloom+xfm)', consts_composite, m_composite_63306D35),
    ('C91275BBAA6E135D', 'composite DOF only', consts_composite, m_dof_only_C91275BB),
    ('2F002AB216A91268', 'composite bloom only', consts_composite, m_bloom_only_2F002AB2),
    ('C6A009EA1DF1BD57', 'composite glare', consts_composite, m_glare_C6A009EA),
    ('F920AF5CD865655E', 'bright pass', consts_generic, m_bright_F920AF5C),
    ('38448F554B8CF69D', 'downsample 4x', consts_generic, m_down4_38448F55),
    ('0D31052586F96765', 'gaussian 15 taps', consts_generic, m_kernel(15)),
    ('7A05ED55B7DFE558', 'DOF blur 8 taps', consts_generic, m_kernel(8)),
]


def main():
    ok_all = True
    for h, name, mk, model in CASES:
        rng = random.Random(hash(h) & 0xffffffff)
        rng.seed(int(h[:8], 16))
        maxerr = 0.0
        tapbad = 0
        for trial in range(TRIALS):
            c = mk(rng)
            uv = (rng.uniform(0, 1), rng.uniform(0, 1))
            tex = Tex(rng)
            got = run_shader(h, c, uv, tex)
            cc = {i: list(v) for i, v in c.items()}
            cc.update(xsim.lits(h))
            exp, taps = model(cc, uv, tex)
            maxerr = max(maxerr, max(abs(g - e) for g, e in zip(got, exp)))
            seen = {(s, round(u, 9), round(v, 9)) for (s, u, v) in tex.log}
            want = {(s, round(u, 9), round(v, 9)) for (s, u, v) in taps}
            if seen != want or len(tex.log) != len(taps):
                tapbad += 1
                if tapbad == 1:
                    print('  tap mismatch', sorted(seen ^ want)[:6])
        ok = maxerr <= 1e-4 and tapbad == 0
        ok_all &= ok
        print('%s %-34s trials=%d max_err=%.3g tap_mismatches=%d %s'
              % (h, name, TRIALS, maxerr, tapbad, 'PASS' if ok else 'FAIL'))
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
