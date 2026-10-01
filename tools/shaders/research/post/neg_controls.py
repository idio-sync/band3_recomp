"""Negative controls for check_post.py: deliberately wrong models must FAIL."""
import check_post as cp

def kernel_xonly(n):
    def m(c, uv, T):
        out = [0.0]*4; taps = set()
        for i in range(n):
            u, v = uv[0]+c[31+i][0], uv[1]+c[31+i][1]; t = T('tf0', u, v); taps.add(('tf0', u, v))
            for k in range(4): out[k] += t[k]*c[47+i][0]
        return out, taps
    return m

def bright_1x(c, uv, T):
    c = dict(c); c[15] = [0.5*c[15][0], 0.5*c[15][1], 0, 0]
    return cp.m_bright_F920AF5C(c, uv, T)

def comp_noabs(c, uv, T):
    real = cp.dof_amount
    cp.dof_amount = lambda c24, d: cp.sat(min(max((1-d)*c24[0]+c24[1], c24[2]), c24[3]))
    try: return cp.m_composite_63306D35(c, uv, T)
    finally: cp.dof_amount = real

def comp_w_negative(rng):           # c24.w < 0 (unreachable in-game): exposes the extra |.| before sat
    c = cp.consts_composite(rng); c[24][3] = -abs(c[24][3]) - 0.1; return c

cp.CASES = [
    ('0D31052586F96765', 'NEG gaussian weights .x only', cp.consts_generic, kernel_xonly(15)),
    ('F920AF5CD865655E', 'NEG bright taps +-1*c15', cp.consts_generic, bright_1x),
    ('63306D35C02782FB', 'NEG composite without |t|', cp.consts_composite, comp_noabs),
    ('63306D35C02782FB', 'EDGE composite c24.w<0', comp_w_negative, cp.m_composite_63306D35),
]
cp.main()

# exact form: a = sat(|min(max(|t|, c24.z), c24.w)|) -- passes even with c24.w < 0
print('-- exact DOF amount with the second |.|:')
cp.dof_amount = lambda c24, d: cp.sat(abs(min(max(abs((1-d)*c24[0]+c24[1]), c24[2]), c24[3])))
cp.CASES = [('63306D35C02782FB', 'EXACT composite c24.w<0', comp_w_negative, cp.m_composite_63306D35),
            ('C91275BBAA6E135D', 'EXACT DOF-only c24.w<0', comp_w_negative, cp.m_dof_only_C91275BB)]
cp.main()
