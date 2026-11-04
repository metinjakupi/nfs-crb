# x87 stack in C locals: inside each straight stretch of a translated function (no label, jump,
# return or call that may look at the CPU), the x87 registers the stretch uses are read into C
# locals once, the stretch works on the locals, and at its end every register it touched is
# written back - value, integer bits, exact flag and tag - with the stack top, exactly as the
# original helpers would have left them. A text pass over regloc's output (registers already in
# locals) into another folder; the input is not touched.
#
#   python3 x87loc.py <gen dir> <out dir> [--regions regions.json]
#
# A stretch is used only when every helper in it is one this pass models (the stack helpers) or
# one that never touches the stack (the status/control-word helpers), every register index is a
# literal, and the registers it touches fit in the eight slots. Anything else is left as it was.
import json, re, sys
from pathlib import Path

gen, out = Path(sys.argv[1]), Path(sys.argv[2])
regions_out = Path(sys.argv[sys.argv.index('--regions') + 1]) if '--regions' in sys.argv else None
MIN_OPS = 4  # stack helper calls a stretch needs before it is worth rewriting

STACK = {'ST', 'fset', 'fdrop', 'fpush', 'fpush_st', 'fpush_int', 'fxch', 'fcopy', 'fstsw', 'fist_i32', 'fist_i16'}
MOVES = {'fdrop', 'fpush', 'fpush_st', 'fpush_int'}  # change the stack top: statements only
# Take the CPU but touch only the status/control words, never the stack: kept as they are.
KEEP = {'fx87', 'fdivz', 'fcom', 'fucom', 'fto_float', 'fx87_exact'}
call_re = re.compile(r'\b([A-Za-z_]\w*)\(\s*c\s*([,)])')
cpu_field = re.compile(r'\bc->(st|fpu_)')
unit_head = re.compile(r'^    \S')

HELPERS = '''
/* x87loc: FLD m32 on a register held in locals. A signalling NaN loads quiet, as x87 does (and as
 * the float-to-double conversion does when the value goes through c->st[]): with the value in a
 * local the compiler may fold a later double-to-float store back to the original float, which
 * would store the signalling pattern. */
static inline double xl_ldf32(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    if ((b & 0x7fc00000u) == 0x7f800000u && (b & 0x003fffffu))
        b |= 0x00400000u;
    memcpy(&f, &b, 4);
    return (double)f;
}
/* x87loc: FIST on a register held in locals (x86.h fist_i32 / fist_i16). */
static inline int32_t xl_fist_i32(X86 *c, double x, uint64_t bits, uint8_t exact) {
    int64_t v = (int64_t)bits;
    if (exact && v >= INT32_MIN && v <= INT32_MAX)
        return (int32_t)v;
    return fto_i32(c, x);
}
static inline int16_t xl_fist_i16(X86 *c, double x, uint64_t bits, uint8_t exact) {
    int64_t v = (int64_t)bits;
    if (exact && v >= INT16_MIN && v <= INT16_MAX)
        return (int16_t)v;
    return fto_i16(c, x);
}
'''

class Bail(Exception):
    pass

def match_paren(s, i):  # s[i] == '('
    depth = 0
    while True:
        ch = s[i]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if not depth:
                return i
        i += 1

def split_args(s):  # top-level commas
    args, depth, cur = [], 0, []
    for ch in s:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(''.join(cur)); cur = []
        else:
            cur.append(ch)
    args.append(''.join(cur))
    return [a.strip() for a in args]

def lit(a):
    if not re.fullmatch(r'\d+u?', a):
        raise Bail('register index ' + a)
    return int(a.rstrip('u'))

def nm(r):
    return ('m%d' % -r) if r < 0 else ('p%d' % r)

class Region:
    def __init__(self):
        self.d = 0
        self.touched = set()
        self.ops = 0

    def slot(self, r):
        self.touched.add(r)
        return nm(r)

    def expr(self, s):
        """Rewrite the helpers inside an expression (no stack moves allowed)."""
        res, i = [], 0
        while True:
            m = call_re.search(s, i)
            if not m:
                res.append(s[i:]); return ''.join(res)
            name = m[1]
            res.append(s[i:m.start()])
            end = match_paren(s, m.end(1))
            inner = s[m.end(1) + 1:end]
            args = split_args(inner)
            if name in MOVES or name in ('fset', 'fxch', 'fcopy'):
                raise Bail(name + ' inside an expression')
            if name == 'ST':
                self.ops += 1
                res.append('X_' + self.slot(self.d + lit(args[1])))
            elif name == 'fstsw':
                self.ops += 1
                res.append('(uint16_t)((c->fpu_sw & (uint16_t)~0x3800u) | (uint16_t)(((T0_ + %du) & 7u) << 11))' % (self.d % 8))
            elif name in ('fist_i32', 'fist_i16'):
                self.ops += 1
                n = self.slot(self.d)
                res.append('xl_%s(c, X_%s, B_%s, E_%s)' % (name, n, n, n))
            elif name in KEEP:
                res.append(name + '(' + ', '.join(['c'] + [self.expr(a) for a in args[1:]]) + ')')
            else:
                raise Bail('helper ' + name)
            i = end + 1

    def statement(self, call, args):
        """A statement-level stack helper: its C replacement."""
        self.ops += 1
        if call == 'fset':
            v = self.expr(args[2]); n = self.slot(self.d + lit(args[1]))
            return '{ double t_ = %s; X_%s = t_; B_%s = 0; E_%s = 0; G_%s = ftag_classify(t_); }' % (v, n, n, n, n)
        if call == 'fpush':
            v = self.expr(args[1]); self.d -= 1; n = self.slot(self.d)
            return '{ double t_ = %s; X_%s = t_; B_%s = 0; E_%s = 0; G_%s = ftag_classify(t_); }' % (v, n, n, n, n)
        if call == 'fpush_int':
            v = self.expr(args[1]); self.d -= 1; n = self.slot(self.d)
            return ('{ int64_t i_ = %s; X_%s = (double)i_; B_%s = (uint64_t)i_; E_%s = 1; G_%s = ftag_classify((double)i_); }'
                    % (v, n, n, n, n))
        if call == 'fpush_st':
            s = self.slot(self.d + lit(args[1])); self.d -= 1; n = self.slot(self.d)
            return '{ X_%s = X_%s; B_%s = B_%s; E_%s = E_%s; G_%s = G_%s; }' % (n, s, n, s, n, s, n, s)
        if call == 'fdrop':
            n = self.slot(self.d); self.d += 1
            return '{ E_%s = 0; G_%s = 3u; }' % (n, n)
        if call == 'fcopy':
            a = self.slot(self.d + lit(args[1])); b = self.slot(self.d + lit(args[2]))
            return '{ X_%s = X_%s; B_%s = B_%s; E_%s = E_%s; G_%s = G_%s; }' % (a, b, a, b, a, b, a, b)
        if call == 'fxch':
            a = self.slot(self.d); b = self.slot(self.d + lit(args[1]))
            return ('{ double x_ = X_%s; uint64_t b_ = B_%s; uint8_t e_ = E_%s; unsigned g_ = G_%s; '
                    'X_%s = X_%s; B_%s = B_%s; E_%s = E_%s; G_%s = G_%s; X_%s = x_; B_%s = b_; E_%s = e_; G_%s = g_; }'
                    % (a, a, a, a, a, b, a, b, a, b, a, b, b, b, b, b))
        raise Bail('statement ' + call)

    def text(self, s):
        """Rewrite a unit's text: statement-level stack helpers become blocks, the rest expressions."""
        res, i = [], 0
        while True:
            m = call_re.search(s, i)
            if not m:
                res.append(s[i:]); return ''.join(res)
            name = m[1]
            end = match_paren(s, m.end(1))
            before = re.sub(r'/\*.*?\*/', '', s[:m.start()], flags=re.S).rstrip()
            at_stmt = (not before or before[-1] in ';{}') and s[end + 1:].lstrip().startswith(';')
            if at_stmt and name in STACK and name not in ('ST', 'fstsw', 'fist_i32', 'fist_i16'):
                res.append(s[i:m.start()])
                res.append(self.statement(name, split_args(s[m.end(1) + 1:end])))
                i = end + 1
                continue
            if name in MOVES or name in ('fset', 'fxch', 'fcopy'):
                raise Bail(name + ' not a statement')
            # An expression statement or initialiser: rewrite from the start of this call.
            res.append(s[i:m.start()])
            res.append(self.expr(s[m.start():end + 1]))
            i = end + 1

    @staticmethod
    def quiet_loads(s):
        """(double)rdf32(a) -> xl_ldf32(rdf32(a)): the float loads through the quieting helper."""
        key = '(double)rdf32('
        while key in s:
            i = s.index(key); j = match_paren(s, i + len(key) - 1)
            s = s[:i] + 'xl_ldf32(rdf32(' + s[i + len(key):j + 1] + ')' + s[j + 1:]
        return s

    def wrap(self, body):
        body = self.quiet_loads(body)
        lo, hi = min(self.touched), max(self.touched)
        if hi - lo > 7:
            raise Bail('more than eight registers')
        pro = ['    { /* x87loc: %d x87 operations on locals */' % self.ops, '    unsigned T0_ = c->fpu_top;']
        epi = ['    { uint16_t tw_ = c->fpu_tag;']
        for r in sorted(self.touched):
            n, p = nm(r), '(T0_ + %du) & 7u' % (r % 8)  # the same physical slot, mod 8
            pro.append('    double X_%s = c->st[%s]; uint64_t B_%s = c->st_bits[%s]; uint8_t E_%s = c->st_exact[%s]; '
                       'unsigned G_%s = ftag_of(c, %s);' % (n, p, n, p, n, p, n, p))
            epi.append('    c->st[%s] = X_%s; c->st_bits[%s] = B_%s; c->st_exact[%s] = E_%s; '
                       'tw_ = (uint16_t)((tw_ & ~(3u << (2u * (%s)))) | (G_%s << (2u * (%s))));' % (p, n, p, n, p, n, p, n, p))
        epi.append('    c->fpu_tag = tw_; c->fpu_top = (T0_ + %du) & 7u; }' % (self.d % 8))
        epi.append('    }')
        return '\n'.join(pro) + '\n' + body + '\n'.join(epi) + '\n'

def units_of(body_lines):
    """Top-level units of a function body: the lines of one statement, its braces balanced
    (comments and strings ignored), so a { } block of several lines is one unit."""
    def balance(line):
        line = re.sub(r'/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '', line)
        return line.count('{') - line.count('}')
    units, i = [], 0
    while i < len(body_lines):
        j, depth = i, balance(body_lines[i])
        while depth > 0:
            j += 1; depth += balance(body_lines[j])
        units.append(body_lines[i:j + 1]); i = j + 1
    return units

def plain(unit):
    """Not a boundary: no label, jump, return, CPU field, or helper outside STACK/KEEP."""
    t = '\n'.join(unit)
    if re.search(r'^\w+:', t, re.M) or re.search(r'\b(goto|return|switch|case|break|continue)\b', t):
        return False
    if cpu_field.search(t) or 'RL_' in t:
        return False
    for m in call_re.finditer(t):
        if m[1] not in STACK and m[1] not in KEEP:
            return False
    return True

stats = {'functions': 0, 'regions': 0, 'ops': 0, 'bailed': {}}
regions = []
def rewrite_function(name, lines):
    us = units_of(lines)
    outl, run = [], []
    def flush():
        if not run:
            return
        stack_calls = sum(len([m for m in call_re.finditer('\n'.join(u)) if m[1] in STACK]) for u in run)
        if stack_calls >= MIN_OPS:
            reg = Region()
            try:
                body = ''.join(reg.text('\n'.join(u) + '\n') for u in run)
                outl.append(reg.wrap(body).rstrip('\n'))
                stats['regions'] += 1; stats['ops'] += reg.ops
                regions.append({'function': name, 'ops': reg.ops, 'orig': '\n'.join('\n'.join(u) for u in run),
                                'new': outl[-1]})
                run.clear(); return
            except Bail as e:
                k = str(e).split(' ')[0]
                stats['bailed'][k] = stats['bailed'].get(k, 0) + 1
        outl.extend('\n'.join(u) for u in run)
        run.clear()
    for u in us:
        if plain(u):
            run.append(u)
        else:
            flush(); outl.append('\n'.join(u))
    flush()
    return outl

fn_re = re.compile(r'^((?:static )?(?:RECOMP_\w+ )*(?:void|uint32_t) (?:fn|body)_[0-9a-f]{8}\(X86 \*c[^)]*\) \{)$')
out.mkdir(parents=True, exist_ok=True)
for f in sorted(gen.glob('chunk_*.c')):
    lines = f.read_text().split('\n')
    res, i = [], 0
    while i < len(lines):
        m = fn_re.match(lines[i])
        if not m:
            res.append(lines[i]); i += 1; continue
        j = i + 1
        while lines[j] != '}':
            j += 1
        stats['functions'] += 1
        name = re.search(r'((?:fn|body)_[0-9a-f]{8})', lines[i])[1]
        res.append(lines[i]); res.extend(rewrite_function(name, lines[i + 1:j])); res.append('}')
        i = j + 1
    (out / f.name).write_text('\n'.join(res))
funcs = (gen / 'funcs.h').read_text()
head, tail = funcs.rstrip().rsplit('#endif', 1)
(out / 'funcs.h').write_text(head + HELPERS + '#endif' + tail + '\n')
for extra in ('x86.h',):
    if (gen / extra).exists():
        (out / extra).write_text((gen / extra).read_text())
if regions_out:
    regions_out.write_text(json.dumps(regions))
print(json.dumps(stats, indent=1))
