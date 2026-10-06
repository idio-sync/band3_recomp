#!/usr/bin/env python3
"""Import function names from the rb3-xenon decomp into band3_functions.toml.

rb3-xenon (https://github.com/freeqaz/rb3-xenon, CC0) targets the same TU5
image as band3. Its scripts/target_symbol_map.json maps addresses to MSVC
mangled names. This script demangles those names (via MSVC's undname.exe),
turns them into C identifiers in the config's Class__Method style, and
renames [functions] entries that are still anonymous rex_sub_XXXXXXXX.

It never changes a name someone already chose. Where both sides name the
same address it reports agreement or disagreement instead.

With --refresh it also keeps the names it imported earlier in step with the
decomp: every address that was anonymous before the first import
(BASELINE_REF) gets rb3-xenon's current name, or goes back to rex_sub_ when
rb3-xenon no longer establishes one. Names that src/ or tests/ use are kept
either way, and listed when the decomp now disagrees.

With --adopt it also replaces hand-written names the decomp disagrees with
outright, for functions over 0x10 bytes that src/ and tests/ don't use. Smaller
functions are left alone: the linker folds identical tiny bodies, so both names
are often right. So are overloads (operator>>, PropSync<T>, ...), whose
decomp name only tells them apart with an address suffix while the
hand-written one usually says which it is (BinStream__ReadVector3).

Skipped on purpose:
  - rows rb3-xenon itself marks as unestablished (_denylist, _icf_arbitrary,
    _bijection_arbitrary) or null, and names mapped to more than one address
  - names that mention a class declared only in the decomp's Dance Central 3
    subsystems (src/system/hamobj, flow, gesture), which RB3 doesn't contain:
    rb3-xenon was partly seeded from the DC3 decomp, and such a name (mostly
    an STL template instance's element type) is a guess
  - 4-byte functions: single-branch thunks are byte-identical to each other,
    so their names are guesses (see _single_branch_thunk_misnames_comment)
  - rex_sub_ names referenced from src/ or tests/, and [rexcrt] addresses

Usage:
  python tools/import_rb3xenon_names.py --map <rb3-xenon>/scripts/target_symbol_map.json
  python tools/import_rb3xenon_names.py --map ... --apply
  python tools/import_rb3xenon_names.py --map ... --refresh --apply
  python tools/import_rb3xenon_names.py --map ... --refresh --adopt --apply

Without --apply it only writes the report. After --apply, rerun
`rexglue codegen band3_manifest.toml` and rebuild.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FUNC_RE = re.compile(r'^(0x[0-9A-Fa-f]+) = \{ name = "([^"]*)", size = (0x[0-9A-Fa-f]+) \}\s*$')
SECTION_RE = re.compile(r'^\s*\[')
REXCRT_RE = re.compile(r'^\s*([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+)')
IDENT_RE = re.compile(r'[A-Za-z_]\w*')
DECL_RE = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
# object-like macros too: ucrt's `#define sys_nerr _sys_nerr` rewrites a same-named declaration
MACRO_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)', re.M)

MAX_NAME_LEN = 120
THUNK_SIZE = 4
# --adopt leaves functions this small alone (see above)
FOLDABLE_SIZE = 0x10

# band3_functions.toml as it was before the first import (ce5bf36); the addresses
# anonymous there are the ones this tool names, and --refresh keeps current
BASELINE_REF = 'ce5bf36~1'
# its name before 2026-10 (BASELINE_REF has it only under this one)
OLD_CONFIG_NAME = 'band3_config.toml'

OPERATORS = {
    '=': 'assign', '==': 'eq', '!=': 'ne', '<': 'lt', '>': 'gt', '<=': 'le',
    '>=': 'ge', '[]': 'index', '()': 'call', '+': 'add', '-': 'sub',
    '*': 'mul', '/': 'div', '%': 'mod', '+=': 'add_assign',
    '-=': 'sub_assign', '*=': 'mul_assign', '/=': 'div_assign',
    '<<': 'shl', '>>': 'shr', '<<=': 'shl_assign', '>>=': 'shr_assign',
    '->': 'arrow', '!': 'not', '++': 'inc', '--': 'dec', '&': 'and',
    '|': 'or', '^': 'xor', '~': 'compl', '&&': 'land', '||': 'lor',
    '&=': 'and_assign', '|=': 'or_assign', '^=': 'xor_assign',
    'new': 'new', 'delete': 'delete', 'new[]': 'new_array',
    'delete[]': 'delete_array',
}

SPECIAL_NAMES = {
    'scalar deleting destructor': 'sdtor',
    'vector deleting destructor': 'vdtor',
    'anonymous namespace': 'anon',
}

CPP_KEYWORDS = set('''
alignas alignof and and_eq asm auto bitand bitor bool break case catch char
char8_t char16_t char32_t class compl concept const consteval constexpr
constinit const_cast continue co_await co_return co_yield decltype default
delete do double dynamic_cast else enum explicit export extern false float for
friend goto if inline int long mutable namespace new noexcept not not_eq
nullptr operator or or_eq private protected public register reinterpret_cast
requires return short signed sizeof static static_assert static_cast struct
switch template this thread_local throw true try typedef typeid typename union
unsigned using virtual void volatile wchar_t while xor xor_eq main
'''.split())


def find_undname():
    roots = [os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
             os.environ.get('ProgramFiles', r'C:\Program Files')]
    hits = []
    for root in roots:
        hits += glob.glob(os.path.join(root, 'Microsoft Visual Studio', '*', '*', 'VC',
                                       'Tools', 'MSVC', '*', 'bin', 'Hostx64', 'x64', 'undname.exe'))
    return sorted(hits)[-1] if hits else None


def host_header_dirs():
    """MSVC, Windows SDK and ReXGlue SDK include dirs, for names a new extern "C" symbol must not shadow."""
    dirs = [os.path.join(REPO, '.rexglue-sdk', 'include')]
    pf86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    pf = os.environ.get('ProgramFiles', r'C:\Program Files')
    for root in (pf86, pf):
        dirs += glob.glob(os.path.join(root, 'Microsoft Visual Studio', '*', '*', 'VC',
                                       'Tools', 'MSVC', '*', 'include'))
    # um and shared carry the Win32 API (CreateFileA, SetFocus, ...) that windows.h pulls in
    for sub in ('ucrt', 'um', 'shared'):
        dirs += glob.glob(os.path.join(pf86, 'Windows Kits', '10', 'Include', '*', sub))
    return [d for d in dirs if os.path.isdir(d)]


def collect_identifiers(dirs, pattern, exts):
    found = set()
    for d in dirs:
        for dirpath, _, files in os.walk(d):
            for f in files:
                if f == 'import_rb3xenon_names.py' or not f.endswith(exts) and exts:
                    continue
                try:
                    with open(os.path.join(dirpath, f), encoding='utf-8', errors='ignore') as fh:
                        found.update(pattern.findall(fh.read()))
                except OSError:
                    pass
    return found


def demangle_all(undname, mangled):
    """Name-only demangle (flag 0x1000). undname reads one symbol per line from a file."""
    mangled = sorted(mangled)
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as tmp:
        tmp.write('\n'.join(mangled) + '\n')
        path = tmp.name
    try:
        out = subprocess.run([undname, '0x1000', path], capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(path)
    lines = out.splitlines()
    if len(lines) < len(mangled):
        sys.exit(f'undname returned {len(lines)} lines for {len(mangled)} symbols')
    return dict(zip(mangled, lines[:len(mangled)]))


def split_scope(name):
    """Split on top-level '::', keeping template arguments and operator names intact."""
    op = None
    idx = name.rfind('::operator')
    if idx >= 0:
        op = name[idx + 2:]
        name = name[:idx]
    elif name.startswith('operator'):
        return [], name
    parts, depth, cur, i = [], 0, '', 0
    while i < len(name):
        c = name[i]
        if c in '<(':
            depth += 1
        elif c in '>)':
            depth -= 1
        if depth == 0 and name.startswith('::', i):
            parts.append(cur)
            cur, i = '', i + 2
            continue
        cur += c
        i += 1
    parts.append(cur)
    if op is not None:
        return parts, op
    return parts[:-1], parts[-1]


def sanitize(component):
    for text, repl in SPECIAL_NAMES.items():
        component = component.replace(f'`{text}\'', repl)
    component = re.sub(r'\b(class|struct|enum|union|const|volatile|__ptr64)\b', '', component)
    component = component.replace('*', 'Ptr').replace('&', 'Ref')
    component = re.sub(r'[^A-Za-z0-9_]+', '_', component)
    return re.sub(r'_+', '_', component).strip('_')


def base_name(component):
    return component.split('<', 1)[0].strip()


def to_identifier(demangled):
    scope, last = split_scope(demangled)
    if last.startswith('operator'):
        op = last[len('operator'):].strip()
        member = 'op_' + OPERATORS.get(op.replace(' ', ''), sanitize(op) or 'unknown')
    elif scope and last == base_name(scope[-1]):
        member = 'ct'
    elif scope and last == '~' + base_name(scope[-1]):
        member = 'dt'
    else:
        member = sanitize(last)
    parts = [sanitize(s) for s in scope] + [member]
    ident = '__'.join(p for p in parts if p)
    if not ident:
        return None
    if ident[0].isdigit():
        ident = 'f_' + ident
    return ident


def normalize(name):
    return re.sub(r'[^a-z0-9]', '', name.lower())


def spelling_variant(cur, demangled):
    """Same function spelled differently: template arguments, operator or destructor wording."""
    plain = re.sub(r"operator.*$|`[^']*'", '', demangled)
    while True:
        stripped = re.sub(r'<[^<>]*>', '', plain)
        if stripped == plain:
            break
        plain = stripped
    a, b = normalize(cur), normalize(plain)
    return bool(b) and len(b) >= 4 and (a.startswith(b) or b.startswith(a))


def is_placeholder(name, addr):
    return (name.startswith(('rex_sub_', 'sub_', 'fn_'))
            or f'{addr:08X}' in name.upper() or 'Function_' in name)


DC3_ONLY_DIRS = ('hamobj', 'flow', 'gesture')
CLASS_DECL_RE = re.compile(r'^\s*(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::|\{)', re.M)


def dc3_only_classes(map_path):
    """Classes the decomp declares only under its Dance Central 3 subsystems."""
    system = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(map_path))), 'src', 'system')
    dc3, elsewhere = set(), set()
    for dirpath, _, files in os.walk(os.path.join(system, '..')):
        rel = os.path.relpath(dirpath, system).replace(os.sep, '/')
        in_dc3 = rel.split('/')[0] in DC3_ONLY_DIRS
        for f in files:
            if not f.endswith(('.h', '.hpp')):
                continue
            try:
                text = open(os.path.join(dirpath, f), encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            (dc3 if in_dc3 else elsewhere).update(CLASS_DECL_RE.findall(text))
    return dc3 - elsewhere


def classify(cur, ident, dem, addr):
    """How an existing name compares with the decomp's."""
    if normalize(cur) == normalize(ident):
        return 'agree'
    if is_placeholder(cur, addr):
        return 'placeholder'
    if spelling_variant(cur, dem):
        return 'variant'
    if normalize(cur.split('__')[-1]) == normalize(ident.split('__')[-1]):
        return 'scope'
    return 'disagree'


def baseline_anonymous(ref, config_path):
    """Addresses that were anonymous in the config at git `ref`."""
    rel = os.path.relpath(os.path.abspath(config_path), REPO).replace(os.sep, '/')
    for path in (rel, OLD_CONFIG_NAME):
        try:
            text = subprocess.run(['git', '-C', REPO, 'show', f'{ref}:{path}'],
                                  capture_output=True, text=True, check=True, encoding='utf-8').stdout
            break
        except (OSError, subprocess.CalledProcessError) as e:
            error = e
    else:
        sys.exit(f'--refresh needs the baseline config {ref}:{rel} from git ({error})')
    anonymous = set()
    for line in text.split('\n'):
        m = FUNC_RE.match(line)
        if m and m.group(2) == f'rex_sub_{int(m.group(1), 16):08X}':
            anonymous.add(int(m.group(1), 16))
    return anonymous


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', required=True, help='rb3-xenon scripts/target_symbol_map.json')
    ap.add_argument('--config', default=os.path.join(REPO, 'band3_functions.toml'))
    ap.add_argument('--report', default=os.path.join(REPO, 'out', 'research', 'rb3xenon_names_report.md'))
    ap.add_argument('--undname', default=None, help='path to undname.exe (auto-detected)')
    ap.add_argument('--apply', action='store_true', help='rewrite band3_functions.toml')
    ap.add_argument('--refresh', action='store_true',
                    help='also update names this tool imported earlier to the decomp\'s current ones')
    ap.add_argument('--adopt', action='store_true',
                    help='replace hand-written names the decomp disagrees with (over 0x10 bytes, unused by src/)')
    ap.add_argument('--baseline-ref', default=BASELINE_REF,
                    help='git ref of the config before the first import (default %(default)s)')
    args = ap.parse_args()

    undname = args.undname or find_undname()
    if not undname or not os.path.isfile(undname):
        sys.exit('undname.exe not found; install MSVC build tools or pass --undname')

    # --- config ---
    with open(args.config, encoding='utf-8') as f:
        lines = f.read().split('\n')
    funcs = {}          # addr -> (line index, name, size)
    rexcrt_addrs = set()
    # names outside [functions] that function names must not reuse
    fixed_names = set()
    section = None
    for i, line in enumerate(lines):
        if SECTION_RE.match(line):
            section = line.strip()
            continue
        if section == '[rexcrt]':
            m = REXCRT_RE.match(line)
            if m:
                rexcrt_addrs.add(int(m.group(2), 16))
                fixed_names.add(m.group(1))
        elif section == '[functions]':
            m = FUNC_RE.match(line)
            if m:
                addr = int(m.group(1), 16)
                funcs[addr] = (i, m.group(2), int(m.group(3), 16))
        elif section == '[[midasm_hook]]':
            m = re.match(r'^\s*name\s*=\s*"([^"]+)"', line)
            if m:
                fixed_names.add(m.group(1))

    # --- rb3-xenon map ---
    with open(args.map, encoding='utf-8') as f:
        smap = json.load(f)
    unestablished = set()
    for key in ('_denylist', '_icf_arbitrary', '_bijection_arbitrary'):
        unestablished.update(int(a, 16) for a in smap.get(key, []))
    allow_dupes = set(smap.get('_internal_linkage_allow', []))
    rows = {int(k, 16): v for k, v in smap.items() if k.startswith('0x') and isinstance(v, str)}
    name_count = Counter(rows.values())

    excluded = Counter()
    candidates = {}
    for addr, mangled in rows.items():
        if addr not in funcs:
            excluded['not a function start in band3_config (data symbol or split mismatch)'] += 1
        elif addr in unestablished:
            excluded['marked unestablished by rb3-xenon (denylist / ICF / bijection)'] += 1
        elif name_count[mangled] > 1 and mangled not in allow_dupes:
            excluded['mangled name maps to more than one address'] += 1
        else:
            candidates[addr] = mangled

    demangled = demangle_all(undname, set(candidates.values()))

    dc3_classes = dc3_only_classes(args.map)
    if dc3_classes:
        dc3_re = re.compile(r'\b(' + '|'.join(sorted(map(re.escape, dc3_classes))) + r')\b')
        for addr in [a for a, m in candidates.items() if dc3_re.search(demangled[m])]:
            del candidates[addr]
            excluded['names a Dance Central 3 class RB3 does not contain'] += 1

    # --- names the new extern "C" symbols must not collide with ---
    referenced = collect_identifiers([os.path.join(REPO, 'src'), os.path.join(REPO, 'tests')],
                                     re.compile(r'\brex_sub_([0-9A-Fa-f]{8})\b'), ())
    referenced = {int(a, 16) for a in referenced}
    src_idents = collect_identifiers([os.path.join(REPO, 'src'), os.path.join(REPO, 'tests')],
                                     IDENT_RE, ('.c', '.cpp', '.h', '.hpp', '.inl'))
    host_dirs = host_header_dirs()
    host_idents = (collect_identifiers(host_dirs, DECL_RE, ('.h', '.hpp', '.inl', ''))
                   | collect_identifiers(host_dirs, MACRO_RE, ('.h', '.hpp', '.inl', '')))
    # DEFINE_REX_FUNC(X) also emits __imp__X, which is the import symbol of a
    # host `_X` (callnewh would collide with ucrt's _callnewh at link time)
    import_clashes = {n[1:] for n in host_idents if n.startswith('_') and len(n) > 1}
    reserved = CPP_KEYWORDS | src_idents | host_idents | import_clashes

    # the addresses this run names: the anonymous ones, and with --refresh
    # also those it named before, except where src/ or tests/ use the name
    anonymous = {a for a, (_, n, _) in funcs.items() if n == f'rex_sub_{a:08X}'}
    managed = set(anonymous)
    kept_in_use = []
    if args.refresh:
        for addr in baseline_anonymous(args.baseline_ref, args.config) & set(funcs):
            if addr in anonymous:
                continue
            if funcs[addr][1] in src_idents:
                kept_in_use.append(addr)
            else:
                managed.add(addr)
    adopted = []
    if args.adopt:
        overloaded = Counter(to_identifier(demangled[m]) for m in candidates.values())
        for addr, mangled in candidates.items():
            _, cur, size = funcs[addr]
            if addr in managed or size <= FOLDABLE_SIZE or cur in src_idents:
                continue
            dem = demangled[mangled]
            ident = to_identifier(dem)
            # a current name that shadows a host declaration is taken over too,
            # so it picks up the rb3_ prefix below even where it agrees with the decomp
            if ident and overloaded[ident] == 1 and (classify(cur, ident, dem, addr) == 'disagree'
                                                     or cur in reserved):
                managed.add(addr)
                adopted.append(addr)
    taken = fixed_names | {n for a, (_, n, _) in funcs.items() if a not in managed}

    desired = {}        # managed addr -> name from the decomp
    skipped = defaultdict(list)
    agree, variants, scope_differs, disagree, placeholder_named = [], [], [], [], []
    prefixed = []
    for addr in sorted(set(candidates) | managed):
        _, cur, size = funcs[addr]
        mangled = candidates.get(addr)
        if mangled is None:
            continue
        dem = demangled[mangled]
        if dem == mangled and mangled.startswith('?'):
            skipped['undname could not demangle'].append((addr, mangled))
            continue
        ident = to_identifier(dem)
        if not ident:
            skipped['no usable identifier'].append((addr, dem))
            continue
        if addr not in managed:
            kind = classify(cur, ident, dem, addr)
            if kind == 'agree':
                agree.append(addr)
            elif kind == 'placeholder':
                placeholder_named.append((addr, cur, dem))
            elif kind == 'variant':
                variants.append((addr, cur, dem))
            elif kind == 'scope':
                scope_differs.append((addr, cur, dem))
            else:
                disagree.append((addr, cur, dem, size))
            continue
        if addr in rexcrt_addrs:
            skipped['address is replaced by [rexcrt]'].append((addr, dem))
        elif addr in referenced:
            skipped['rex_sub_ name referenced in src/ or tests/'].append((addr, dem))
        elif size <= THUNK_SIZE:
            skipped['4-byte thunk (name not established)'].append((addr, dem))
        else:
            if ident in reserved or (ident.startswith('_') and len(ident) > 1 and (ident[1] == '_' or ident[1].isupper())):
                prefixed.append((addr, ident))
                ident = 'rb3_' + ident.lstrip('_')
            if len(ident) > MAX_NAME_LEN:
                ident = ident[:MAX_NAME_LEN].rstrip('_')
            desired[addr] = ident

    # Overloads and template instantiations collapse to the same identifier;
    # suffix every member of a clash with its address so names stay unique and stable.
    counts = Counter(desired.values())
    suffixed = 0
    for addr, ident in desired.items():
        if counts[ident] > 1 or ident in taken:
            desired[addr] = f'{ident}_{addr:08X}'
            suffixed += 1

    # every managed address ends up with its decomp name, or anonymous; an
    # adopted hand-written name the decomp can't replace stays as it is
    adopted_set = set(adopted)
    final = {a: desired.get(a, funcs[a][1] if a in adopted_set else f'rex_sub_{a:08X}')
             for a in managed}
    renames = {a: n for a, n in final.items() if n != funcs[a][1]}
    named = [a for a in renames if a in anonymous]
    adopted = [a for a in renames if a in adopted_set]
    updated = [a for a in renames if a not in anonymous and a not in adopted_set and a in desired]
    withdrawn = [a for a in renames if a not in anonymous and a not in desired]

    all_names = Counter(taken) + Counter(final.values())
    dupes = [n for n, c in all_names.items() if c > 1 and n not in fixed_names]
    if dupes:
        sys.exit(f'internal error: {len(dupes)} non-unique names, e.g. {dupes[:5]}')
    bad = [n for n in final.values() if not re.fullmatch(r'[A-Za-z_]\w*', n)]
    if bad:
        sys.exit(f'internal error: invalid identifiers, e.g. {bad[:5]}')

    # --- report ---
    commit = ''
    map_repo = os.path.dirname(os.path.dirname(os.path.abspath(args.map)))
    try:
        commit = subprocess.run(['git', '-C', map_repo, 'rev-parse', 'HEAD'],
                                capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    anon_before = len(anonymous)
    r = []
    r.append('# rb3-xenon name import report\n')
    r.append(f'- rb3-xenon commit: `{commit or "unknown"}`')
    r.append(f'- Map rows with a name: {len(rows)}; usable as function names: {len(candidates)}')
    r.append(f'- band3_config functions: {len(funcs)}, anonymous before: {anon_before}')
    r.append(f'- **Named: {len(named)}** anonymous functions ({suffixed} address-suffixed for '
             f'uniqueness, {len(prefixed)} prefixed `rb3_` to avoid host/src/keyword clashes)')
    if args.adopt:
        r.append(f'- **Adopted: {len(adopted)}** hand-written names replaced by the decomp\'s')
    if args.refresh:
        r.append(f'- Refresh against `{args.baseline_ref}`: **{len(updated)} earlier imports updated** '
                 f'to the decomp\'s current name, **{len(withdrawn)} withdrawn** (back to rex_sub_), '
                 f'{len(kept_in_use)} kept because src/ or tests/ use them')
    r.append(f'- Already named, agree: {len(agree)}; spelling variants: {len(variants)}; '
             f'same method, different scope: {len(scope_differs)}; '
             f'**disagree: {len(disagree)}**; existing name is a placeholder: {len(placeholder_named)}')
    r.append(f'- Applied: {"yes" if args.apply else "no (dry run)"}\n')
    r.append('## Excluded map rows\n')
    for reason, n in excluded.most_common():
        r.append(f'- {reason}: {n}')
    r.append('\n## Skipped renames\n')
    for reason, items in skipped.items():
        r.append(f'- {reason}: {len(items)}')
        if len(items) <= 10:
            for addr, dem in items:
                r.append(f'  - `0x{addr:08X}` would be `{dem}`')
    r.append('\n## Disagreements (existing name kept)\n')
    r.append('Check these by hand. Small functions (<= 0x10 bytes) are often folded by the linker, '
             'so both names can be right for the same body.\n')
    r.append('| Address | Size | band3_config | rb3-xenon |')
    r.append('|---|---|---|---|')
    for addr, cur, dem, size in disagree:
        r.append(f'| 0x{addr:08X} | 0x{size:X} | `{cur}` | `{dem}` |')
    r.append('\n## Same method, different scope (existing name kept)\n')
    r.append('| Address | band3_config | rb3-xenon |')
    r.append('|---|---|---|')
    for addr, cur, dem in scope_differs:
        r.append(f'| 0x{addr:08X} | `{cur}` | `{dem}` |')
    r.append('\n## Spelling variants (existing name kept)\n')
    r.append('| Address | band3_config | rb3-xenon |')
    r.append('|---|---|---|')
    for addr, cur, dem in variants:
        r.append(f'| 0x{addr:08X} | `{cur}` | `{dem}` |')
    r.append('\n## Placeholder names that rb3-xenon could replace (existing name kept)\n')
    r.append('| Address | band3_config | rb3-xenon |')
    r.append('|---|---|---|')
    for addr, cur, dem in placeholder_named:
        r.append(f'| 0x{addr:08X} | `{cur}` | `{dem}` |')
    if args.adopt:
        r.append('\n## Hand-written names replaced (--adopt)\n')
        r.append('| Address | Size | Was | Now |')
        r.append('|---|---|---|---|')
        for addr in sorted(adopted):
            r.append(f'| 0x{addr:08X} | 0x{funcs[addr][2]:X} | `{funcs[addr][1]}` | `{renames[addr]}` |')
    if args.refresh:
        r.append('\n## Earlier imports updated\n')
        r.append('| Address | Was | Now |')
        r.append('|---|---|---|')
        for addr in sorted(updated):
            r.append(f'| 0x{addr:08X} | `{funcs[addr][1]}` | `{renames[addr]}` |')
        r.append('\n## Earlier imports withdrawn (rb3-xenon no longer establishes a name)\n')
        r.append('| Address | Was |')
        r.append('|---|---|')
        for addr in sorted(withdrawn):
            r.append(f'| 0x{addr:08X} | `{funcs[addr][1]}` |')
        r.append('\n## Kept because src/ or tests/ use them\n')
        r.append('| Address | band3_config | rb3-xenon |')
        r.append('|---|---|---|')
        for addr in sorted(kept_in_use):
            mangled = candidates.get(addr)
            dem = demangled.get(mangled, '(none)') if mangled else '(none)'
            r.append(f'| 0x{addr:08X} | `{funcs[addr][1]}` | `{dem}` |')
    r.append('\n## Prefixed names\n')
    for addr, ident in prefixed:
        r.append(f'- `0x{addr:08X}` {ident} -> {final[addr]}')
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    with open(args.report, 'w', encoding='utf-8') as f:
        f.write('\n'.join(r) + '\n')
    tsv = os.path.splitext(args.report)[0] + '_renames.tsv'
    with open(tsv, 'w', encoding='utf-8') as f:
        f.write('address\tsize\tnew_name\tdemangled\n')
        for addr in sorted(renames):
            dem = demangled[candidates[addr]] if addr in candidates else '(withdrawn)'
            f.write(f'0x{addr:08X}\t0x{funcs[addr][2]:X}\t{renames[addr]}\t{dem}\n')

    if args.apply:
        for addr, ident in renames.items():
            i, cur, _ = funcs[addr]
            lines[i] = lines[i].replace(f'name = "{cur}"', f'name = "{ident}"', 1)
        with open(args.config, 'w', encoding='utf-8', newline='') as f:
            f.write('\n'.join(lines))

    refreshed = (f'; refreshed {len(updated)}, withdrew {len(withdrawn)}, kept {len(kept_in_use)} in use'
                 if args.refresh else '')
    if args.adopt:
        refreshed += f'; adopted {len(adopted)}'
    print(f'named {len(named)} of {anon_before} anonymous functions{refreshed}'
          f'{"" if args.apply else " (dry run)"}; '
          f'{len(disagree)} disagreements, {len(scope_differs)} scope differences; report: {args.report}')


if __name__ == '__main__':
    main()
