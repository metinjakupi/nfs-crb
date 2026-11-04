# Registers and flags in C locals: keeps the guest's integer registers and arithmetic flags in
# locals inside each translated function, writing them back to the X86 struct only around
# anything that may read or write them (calls, returns, helpers that take the CPU). A text pass
# over the generated chunks (translate.py output) into another folder; the input is not touched.
#
#   python3 regloc.py <gen dir> <out dir> regs-only
#
# regs-only is the variant the 2026-11-03 fps release ships ("targeted"): registers are written
# back around ordinary calls, flags are carried in locals across jumps and returns but not across
# calls (except into FLAG_CALLEES, the callees found to read flags at entry). Without it, flags
# are written back at every sync point as well (the safe variant, slower).
# Not yet a build step: the release ran this on source/build/recomp/gen and compiled its output
# with the official chunk flags (plans/threads-and-locks.md).
import re, sys
from pathlib import Path
gen, out = Path(sys.argv[1]), Path(sys.argv[2])
REGS_ONLY = len(sys.argv) > 3 and sys.argv[3] == 'regs-only'
# Called (not jumped to) and reading flags at entry (a scan of every called function).
FLAG_CALLEES = {'008d278a'}
out.mkdir(parents=True, exist_ok=True)
hdr = (gen / 'x86.h').read_text() + ((Path(__file__).resolve().parents[2] / 'runtime/intrinsics.h').read_text())

# Helpers that only touch x87/SSE state: calls to them are not sync points. Checked below.
PURE = {'ST', 'fset', 'fdrop', 'fx87', 'fpush', 'fto_float', 'fstsw', 'fcom', 'fpush_st', 'fpush_int', 'fxch',
        'fdivz', 'fucom', 'fcopy', 'fx87_exact', 'fist_i32', 'fist_i16', 'fist_i64', 'fround_cw',
        'xmm_set_f32', 'xmm_f32'}
def body_of(name):
    m = re.search(r'^[^\n;#]*\b' + name + r'\s*\((?:const )?X86 \*c[^)]*\)\s*\{', hdr, re.M)
    if not m:
        m = re.search(r'#define ' + name + r'\(c,[^\n]*', hdr)
        return m[0] if m else None
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(hdr[i], 0); i += 1
    return hdr[m.start():i]
for n in PURE:
    b = body_of(n)
    assert b is not None, n
    assert '->r[' not in b and 'eflags' not in b and 'r[R_' not in b, n
def returns_void(name):
    m = re.search(r'^\s*(?:static\s+inline\s+|RECOMP_HOT_INLINE\s+|RECOMP_COLD_OUTLINE\s+|extern\s+)*([\w\s\*]+?)\b' + name + r'\s*\((?:const )?X86 \*c', hdr, re.M)
    # Macro-made helpers (shift/rotate families) are not matched; they all return a value.
    return bool(m) and m[1].split()[-1] == 'void'

FLAGS = ['cf', 'zf', 'sf', 'of', 'pf', 'af', 'df']
SO = '(memcpy(c->r, R_, sizeof R_), ' + ', '.join(f'c->eflags_{f} = F_{f}' for f in FLAGS) + ', (void)0)'
SI = '(memcpy(R_, c->r, sizeof R_), ' + ', '.join(f'F_{f} = c->eflags_{f}' for f in FLAGS) + ', (void)0)'
funcs = (gen / 'funcs.h').read_text()
funcs = funcs.replace('#endif\n', '#endif\n', 1)
tail = funcs.rstrip().rsplit('#endif', 1)
funcs = tail[0] + ('/* regloc prototype: registers and flags in locals (regloc.py) */\n#include <string.h>\n'
                   f'#define RL_SO {SO}\n#define RL_SI {SI}\n'
                   '#define RL_SOR (memcpy(c->r, R_, sizeof R_), (void)0)\n#define RL_SIR (memcpy(R_, c->r, sizeof R_), (void)0)\n'
                   '#define RL_DECL uint32_t R_[8], ' + ', '.join(f'F_{f}' for f in FLAGS) + '; RL_SI\n#endif\n')
(out / 'funcs.h').write_text(funcs)
(out / 'x86.h').write_text((gen / 'x86.h').read_text())

def match_paren(s, i):  # s[i] == '('
    depth = 0
    while True:
        ch = s[i]
        if ch == '(': depth += 1
        elif ch == ')':
            depth -= 1
            if not depth: return i
        i += 1

call_re = re.compile(r'\b([A-Za-z_]\w*)\(c([,)])')
void_cache = {}
def rewrite_calls(line):
    res, i = [], 0
    while True:
        m = call_re.search(line, i)
        if not m: res.append(line[i:]); return ''.join(res)
        name = m[1]
        if name in PURE:
            res.append(line[i:m.end()]); i = m.end(); continue
        start = m.start(); end = match_paren(line, m.end(1))
        inner = rewrite_calls(line[m.end(1) + 1:end])
        call = f'{name}({inner})'
        if name not in void_cache: void_cache[name] = returns_void(name)
        # A call that is a whole statement needs no value, whatever its type.
        whole = not line[:start].strip(' {') and line[end + 1:].lstrip().startswith(';')
        if void_cache[name] or whole: res.append(line[i:start] + f'(RL_SO, {call}, RL_SI)')
        else: res.append(line[i:start] + f'({{ RL_SO; __typeof__({call}) rl_v_ = {call}; RL_SI; rl_v_; }})')
        if REGS_ONLY and name == 'recomp_call':
            res[-1] = res[-1].replace('RL_SO', 'RL_SOR')
        i = end + 1

stats = {'functions': 0, 'skipped': 0}
sig = re.compile(r'^(?:static )?void (?:fn|body)_[0-9a-f]{8}\((?:X86 \*c|RECOMP_BODY_ARGS)\) \{$')
for src in sorted(gen.glob('chunk_*.c')) + [gen / 'table.c']:
    lines = src.read_text().split('\n'); o = []; inside = False
    for ln in lines:
        if not inside and sig.match(ln):
            inside = True; stats['functions'] += 1
            o.append(ln); o.append('    RL_DECL;'); continue
        if inside and ln == '}':
            inside = False; o.append(ln); continue
        if not inside or src.name == 'table.c':
            o.append(ln); continue
        code, sep, comment = ln.partition('/*')
        code = code.replace('c->r[', 'R_[')
        code = re.sub(r'c->eflags_(cf|zf|sf|of|pf|af|df)\b', r'F_\1', code)
        code = rewrite_calls(code)
        so = 'RL_SOR' if REGS_ONLY else 'RL_SO'
        code = re.sub(r'\b(CALL_FN\(([0-9a-f]{8})\));', lambda m: '{ ' + ('RL_SO' if m[2] in FLAG_CALLEES else so) + '; ' + m[1] + '; RL_SI; }', code)
        code = re.sub(r'\b(TAIL_FN\([^)]*\));', r'{ RL_SO; \1; }', code)
        code = re.sub(r'\b(RECOMP_BODY_CALL\([^)]*\));', r'{ RL_SO; \1; }', code)
        code = re.sub(r'\breturn;', '{ RL_SO; return; }', code)
        assert 'return ' not in code or 'RL_SO' in code, (src.name, ln)
        o.append(code + sep + comment)
    (out / src.name).write_text('\n'.join(o))
print(stats, 'sync helpers:', sorted(void_cache))
