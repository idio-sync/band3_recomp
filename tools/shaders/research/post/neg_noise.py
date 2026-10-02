"""Negative controls for check_noise.py: deliberately wrong noise models must FAIL.

The overlay picked per channel instead of by the luminance, the weight without
its 6.75, and the taps' arithmetic mean instead of their geometric mean.
Exits 1 if any of them passes.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_noise as cn

orig = cn.noise


def per_channel(c, uv, T, rgb, midtone_const=6.75):
    a, b = cn.noise_taps(c, uv)
    N1, N2 = T('tf13', *a), T('tf13', *b)
    n = [math.sqrt(abs(N1[k] * N2[k])) for k in range(3)]
    L = 0.30 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2]
    ov = [2 * n[k] * rgb[k] if rgb[k] <= 0.5 else 1 - 2 * (1 - n[k]) * (1 - rgb[k])
          for k in range(3)]
    w = 6.75 * c[113][3] * L * (1 - L) ** 2
    return [rgb[k] + w * (ov[k] - rgb[k]) for k in range(3)], {('tf13',) + a, ('tf13',) + b}


def no_const(c, uv, T, rgb, midtone_const=6.75):
    return orig(c, uv, T, rgb, 1.0)


def avg(c, uv, T, rgb, midtone_const=6.75):
    a, b = cn.noise_taps(c, uv)
    N1, N2 = T('tf13', *a), T('tf13', *b)
    n = [(N1[k] + N2[k]) / 2 for k in range(3)]
    L = 0.30 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2]
    if L <= .5:
        ov = [2 * n[k] * rgb[k] for k in range(3)]
    else:
        ov = [1 - 2 * (1 - n[k]) * (1 - rgb[k]) for k in range(3)]
    w = 6.75 * c[113][3] * L * (1 - L) ** 2
    return [rgb[k] + w * (ov[k] - rgb[k]) for k in range(3)], {('tf13',) + a, ('tf13',) + b}


def main():
    ok = True
    for name, f in [('per-channel overlay', per_channel), ('no 6.75', no_const),
                    ('mean not geomean', avg)]:
        cn.noise = f
        print('negative control:', name)
        passed = cn.main(40)
        print('  expect FAIL ->', 'PASS (bad)' if passed else 'FAIL')
        ok &= not passed
    cn.noise = orig
    return ok


if __name__ == '__main__':
    sys.exit(0 if main() else 1)
