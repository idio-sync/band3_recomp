"""Numeric interpreter for Xenia's Xenos ucode disassembly (.ucode.vert/.frag).

Runs the straight-line ALU and fetch instructions of a dumped shader on given
inputs so a hypothesised formula can be checked against what the game's shader
really computes. Fetches return caller-supplied values (texture samples / vertex
attributes); of control flow, only exec blocks and loops (a loop constant's
count from State.loops; no breaks or predicated jumps) are supported.

Its inputs come from your own copy of the game and stay out of the repo (see
README.md): DUMP is where a --dump_shaders run wrote the .ucode files, WORK holds
the ark's raw shader blobs, and LITS is the lits.json lit.py and vlit.py make
from them. BAND3_SHADER_DUMP, BAND3_SHADER_WORK and BAND3_SHADER_LITS override them.
"""
import re, math, json, os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
# absolute, as lit.py and vlit.py chdir into DUMP
DUMP = os.path.abspath(os.environ.get('BAND3_SHADER_DUMP', os.path.join(REPO, 'out', 'shaders')))
WORK = os.path.abspath(os.environ.get('BAND3_SHADER_WORK', os.path.join(REPO, 'out', 'shader_research')))
LITS = os.path.abspath(os.environ.get('BAND3_SHADER_LITS', os.path.join(WORK, 'lits.json')))
_lits = None

def lits(h):
    global _lits
    if _lits is None:
        _lits = json.load(open(LITS))
    v = _lits.get(h, {})
    return {int(k): list(t) for k, t in v.get('lits', {}).items()}

INSTR = re.compile(r'^/\*\s*(\d+)\s*\*/\s+(.*)$')
COISSUE = re.compile(r'^\s+\+\s+(.*)$')

def load(h, kind='frag'):
    path = os.path.join(DUMP, 'shader_%s.ucode.%s' % (h, kind))
    prog = []
    for line in open(path):
        line = line.rstrip()
        if not line.strip():
            continue
        m = re.match(r'^/\*\s*([\d.]+)\s*\*/\s+(.*)$', line)
        if m:
            if '.' in m.group(1):
                # control flow (exec/alloc/cnop/...): only loops matter
                body = m.group(2).strip()
                if body.startswith('loop '):
                    prog.append(['@loop', body.split()[1].rstrip(',')])
                elif body.startswith('endloop '):
                    prog.append(['@endloop'])
                continue
            body = m.group(2).strip()
            if body == 'serialize':
                continue
            prog.append([body])
            continue
        m = COISSUE.match(line)
        if m:
            prog[-1].append(m.group(1).strip())
            continue
        s = line.strip()
        if s.startswith('label'):
            continue  # a loop's target: its loop and endloop are in prog
        if s.startswith('loop') or s.startswith('jmp'):
            raise NotImplementedError('control flow: ' + s)
        # an instruction after a 'serialize' line, maybe predicated
        op = s.split()[1] if s.split()[0] in ('(p0)', '(!p0)') and len(s.split()) > 1 else s.split()[0]
        if s and op.replace('_sat', '') in OPS:
            prog.append([s])
            continue
        raise ValueError('unparsed: ' + line)
    return prog

OPS = set('''cube add mul mad max min dp3 dp4 dp2add sge sgt seq sne cndeq cndge cndgt frc trunc floor
 tfetch2D tfetchCube tfetch3D vfetch_full vfetch_mini'''.split())

VOPS = set('cube add mul mad max min dp3 dp4 dp2add sge sgt seq sne cndeq cndge cndgt frc trunc floor'.split())

def comp(c):
    return 'xyzw'.index(c)

class State:
    def __init__(self, consts, regs=None, fetch=None, loops=None):
        self.c = {k: list(v) for k, v in consts.items()}
        # loop constants by name ('i31'): each loop's iteration count
        self.loops = dict(loops or {})
        self.r = {}
        for k, v in (regs or {}).items():
            self.r[k] = list(v)
        self.o = {}
        self.ps = 0.0
        self.a0 = 0
        self.fetch = fetch or (lambda kind, unit, coord, line: [0.0, 0.0, 0.0, 0.0])
        self.log = []

    def reg(self, name):
        if name.startswith('c['):
            m = re.match(r'c\[(\d+)\+a0\]', name)
            return self.c.get(int(m.group(1)) + self.a0, [0.0] * 4)
        if name[0] == 'c':
            return self.c.get(int(name[1:]), [0.0] * 4)
        if name.startswith('r'):
            return self.r.setdefault(int(name[1:]), [0.0] * 4)
        raise ValueError(name)

SRC = re.compile(r'^(-?)(r_abs\[(\d+)\]|r\d+|c\d+|c\[\d+\+a0\])(?:\.([xyzw]+))?$')

def read_src(st, s):
    m = SRC.match(s)
    if not m:
        raise ValueError('src ' + s)
    neg, name, absn, sw = m.groups()
    sw = sw or 'xyzw'
    if absn is not None:
        v = [abs(x) for x in st.reg('r' + absn)]
    else:
        v = st.reg(name)
    vals = [v[comp(ch)] for ch in sw]
    if neg:
        vals = [-x for x in vals]
    return vals

def sat(x):
    return min(max(x, 0.0), 1.0)

def write(st, dest, res, satf):
    name, _, mask = dest.partition('.')
    if not _:
        mask = 'xyzw'
    if name.startswith('o') or name.startswith('oC'):
        tgt = st.o.setdefault(name, [0.0] * 4)
    else:
        tgt = st.reg(name)
    if mask in ('_', ''):
        return
    for i, ch in enumerate(mask[:4]):
        if ch == '_':
            continue
        if ch == '0':
            tgt[i] = 0.0
        elif ch == '1':
            tgt[i] = 1.0
        else:
            v = res[comp(ch)] if ch != 'xyzw'[i] else res[i]
            tgt[i] = sat(v) if satf else v

def flog2(x):
    if x <= 0:
        return -math.inf if x == 0 else float('nan')
    return math.log2(x)

def fexp2(x):
    try:
        return 2.0 ** x
    except OverflowError:
        return math.inf

def vec_op(op, a):
    if op == 'add': return [a[0][i] + a[1][i] for i in range(4)]
    if op == 'mul': return [a[0][i] * a[1][i] for i in range(4)]
    if op == 'mad': return [a[0][i] * a[1][i] + a[2][i] for i in range(4)]
    if op == 'max': return [max(a[0][i], a[1][i]) for i in range(4)]
    if op == 'min': return [min(a[0][i], a[1][i]) for i in range(4)]
    if op == 'dp3': return [sum(a[0][i] * a[1][i] for i in range(3))] * 4
    if op == 'dp4': return [sum(a[0][i] * a[1][i] for i in range(4))] * 4
    if op == 'dp2add': return [a[0][0] * a[1][0] + a[0][1] * a[1][1] + a[2][0]] * 4
    if op == 'sge': return [1.0 if a[0][i] >= a[1][i] else 0.0 for i in range(4)]
    if op == 'sgt': return [1.0 if a[0][i] > a[1][i] else 0.0 for i in range(4)]
    if op == 'seq': return [1.0 if a[0][i] == a[1][i] else 0.0 for i in range(4)]
    if op == 'sne': return [1.0 if a[0][i] != a[1][i] else 0.0 for i in range(4)]
    if op == 'cndeq': return [a[1][i] if a[0][i] == 0 else a[2][i] for i in range(4)]
    if op == 'cndge': return [a[1][i] if a[0][i] >= 0 else a[2][i] for i in range(4)]
    if op == 'cndgt': return [a[1][i] if a[0][i] > 0 else a[2][i] for i in range(4)]
    if op == 'cube':
        # record the direction; return it unchanged so tfetchCube logs it
        d = [a[1][1], a[1][0], a[0][2]]  # src0.zzxy, src1.yxzz -> (x, y, z)
        return [a[0][2], a[0][3], a[0][0], 0.0]
    if op == 'frc': return [x - math.floor(x) for x in a[0]]
    if op == 'trunc': return [float(math.trunc(x)) for x in a[0]]
    if op == 'floor': return [float(math.floor(x)) for x in a[0]]
    raise NotImplementedError(op)

def sca_op(st, op, a):
    ps = st.ps
    x = a[0] if a else 0.0
    y = a[1] if len(a) > 1 else x
    if op == 'retain_prev': return ps
    if op.startswith('setp_'):
        cond = {'setp_gt': x > 0, 'setp_ge': x >= 0, 'setp_eq': x == 0, 'setp_ne': x != 0}.get(op.replace('_push', ''), None)
        if cond is None: raise NotImplementedError(op)
        st.p0 = cond
        return 0.0 if cond else 1.0
    if op in ('adds',): return x + y
    if op == 'adds_prev': return x + ps
    if op == 'subs': return x - y
    if op == 'subs_prev': return x - ps
    if op == 'muls': return x * y
    if op == 'muls_prev': return x * ps
    if op == 'maxs': return max(x, y)
    if op == 'mins': return min(x, y)
    if op == 'maxas':
        st.a0 = int(math.floor(x + 0.5))
        return max(x, y)
    if op == 'sges': return 1.0 if x >= 0 else 0.0
    if op == 'sgts': return 1.0 if x > 0 else 0.0
    if op == 'seqs': return 1.0 if x == 0 else 0.0
    if op == 'snes': return 1.0 if x != 0 else 0.0
    if op == 'frcs': return x - math.floor(x)
    if op == 'truncs': return float(math.trunc(x))
    if op == 'floors': return float(math.floor(x))
    if op == 'exp': return fexp2(x)
    if op in ('log', 'logc'):
        v = flog2(x)
        return v
    if op in ('rcp', 'rcpc', 'rcpf'): return math.inf if x == 0 else 1.0 / x
    if op in ('rsq', 'rsqc', 'rsqf'): return math.inf if x == 0 else 1.0 / math.sqrt(x) if x > 0 else float('nan')
    if op == 'sqrt': return math.sqrt(x) if x >= 0 else float('nan')
    if op == 'sin': return math.sin(x)
    if op == 'cos': return math.cos(x)
    if op == 'mulsc': return x * y
    if op == 'addsc': return x + y
    if op == 'subsc': return x - y
    raise NotImplementedError(op)

def split_args(s):
    return [t.strip() for t in s.split(',')]

def unroll(prog, st):
    """prog with each loop's body repeated its loop constant's count"""
    out, i = [], 0
    while i < len(prog):
        if prog[i][0] == '@loop':
            # the body, up to the matching endloop
            depth, j = 1, i + 1
            while depth:
                depth += {'@loop': 1, '@endloop': -1}.get(prog[j][0], 0)
                j += 1
            out += unroll(prog[i + 1:j - 1], st) * st.loops[prog[i][1]]
            i = j
        else:
            out.append(prog[i])
            i += 1
    return out

def run(prog, st):
    for group in unroll(prog, st):
        # a predicated group the predicate turns off: skipped, its fetches
        # too (so the caller's fetch log has only the taps the shader takes)
        first = group[0]
        if first.startswith(('(p0) ', '(!p0) ')) and \
                bool(getattr(st, 'p0', False)) != first.startswith('(p0)'):
            continue
        # gather all reads first (parallel issue)
        pending = []
        new_ps = None
        pred = None
        for k, ins in enumerate(group):
            if ins.startswith('(p0) ') or ins.startswith('(!p0) '):
                pred = ins.startswith('(p0)')
                ins = ins.split(' ', 1)[1]
            op, _, rest = ins.partition(' ')
            args = split_args(rest.split(', FetchValidOnly')[0]) if not rest.startswith('r') or True else None
            satf = op.endswith('_sat')
            bop = op[:-4] if satf else op
            if bop.startswith(('tfetch', 'vfetch', 'getWeights')):
                dest = args[0]
                coord = read_src(st, args[1]) if SRC.match(args[1]) else None
                unit = args[2] if len(args) > 2 else ''
                if bop.startswith('getWeights'):
                    unit = 'w' + unit
                val = st.fetch(bop, unit, coord, ins)
                st.log.append((bop, unit, coord))
                name, dot_, mask = dest.partition('.')
                if not dot_:
                    mask = 'xyzw'
                # fetch dest: per dest component, source component or constant
                pending.append(('fetch', name, mask, val))
                continue
            if k == 0 and bop in VOPS:
                srcs = [read_src(st, s) for s in args[1:]]
                res = vec_op(bop, srcs)
                if bop == 'cube':
                    st.log.append(('cube', [srcs[0][2], srcs[0][3], srcs[0][0]]))
                pending.append(('alu', args[0], res, satf))
            else:
                srcs = []
                for s in args[1:]:
                    srcs.extend(read_src(st, s))
                v = sca_op(st, bop, srcs)
                if satf:
                    v = sat(v)
                new_ps = v
                pending.append(('alu', args[0], [v] * 4, False))
        if pred is not None and bool(getattr(st, 'p0', False)) != pred:
            continue
        for p in pending:
            if p[0] == 'fetch':
                _, name, mask, val = p
                tgt = st.reg(name)
                for i, ch in enumerate(mask[:4]):
                    if ch == '_':
                        continue
                    tgt[i] = 0.0 if ch == '0' else 1.0 if ch == '1' else val[comp(ch)]
            else:
                _, dest, res, satf = p
                write(st, dest, res, satf)
        if new_ps is not None:
            st.ps = new_ps
    return st
