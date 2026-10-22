"""Arm64 functions of a guest library as C for x86_64: Horizon code that runs native instead of
translated (native/berberis_proxy_libshell).

    python tools/lift_arm64.py LIB OUT.c ADDR=SIZE... [--extern NAME=KIND...] [--extern ADDR=KIND...]

Digitalis keeps a guest's registers in memory and checks its float math instruction by instruction,
so hot leaf math such as VrShell's ray hit-testing runs many times slower translated than compiled.
This turns each listed function, instruction by instruction, into a C function on locals that a
compiler keeps in registers:

  * `void lift_<addr>(uint8_t *ts)`, ts the translator's ThreadState (CPUState first: x0-x30 at 0,
    sp at 0x108, v0-v31 at 0x110; the guest TLS pointer, which mrs tpidr_el0 reads, at 0x358). It
    reads every register at entry and writes every one back at its ret, so it does what the guest
    function does to the registers, its stack (the guest's: sp) and memory, as the translator's
    trampolines expect of a host function standing in for guest code (they then return to x30).
  * Calls to other listed functions are C calls, the registers going through ts. Calls to externs
    are host calls (sincosf, atan2f: the PC's libm, as Prism's libm proxy makes the guest's) or
    fatal (an assert's report, abort, the stack check).
  * Float semantics are arm64's: fmadd, fmla and the like are fused (fmaf, so the lifted functions
    are built for FMA, and the runtime checks the CPU has it before using them), writes to s registers clear the rest of the vector register, .2s operations clear
    its upper half. One known difference: a NaN an operation creates from non-NaN operands (0 * inf)
    is x86's default NaN, 0xffc00000, where arm64's is 0x7fc00000 (the sign bit; comparisons and
    the hit-test results built from them don't see it).
  * Branches out of a function, indirect calls and instructions it doesn't know are refused
    (Unsupported), so a function is lifted whole or not at all.

The output also has the table native/berberis_proxy_libshell's runtime checks before using any of
it: each function's offset, size, FNV-1a hash of its bytes, and the library's GNU build ID. Each
basic block starts with COVER(addr), empty unless the includer defines it, and the memory accesses
go through LD32...STQ, which it may also define: native/berberis_proxy_libshell/check records and
replays calls through them. Needs capstone (as tools/reverse.py does).
"""
import re
import struct
import sys

import capstone

TLS_OFFSET = 0x358  # ThreadState::tls
CONDS = {
    'eq': 'z', 'ne': '!z', 'hs': 'cf', 'cs': 'cf', 'lo': '!cf', 'cc': '!cf', 'mi': 'n', 'pl': '!n',
    'vs': 'vf', 'vc': '!vf', 'hi': '(cf && !z)', 'ls': '(!cf || z)', 'ge': '(n == vf)', 'lt': '(n != vf)',
    'gt': '(!z && n == vf)', 'le': '(z || n != vf)', 'al': '1',
}


class Unsupported(Exception):
    pass


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(s):
    s = s.lstrip('#')
    return int(s, 16) if s.lower().startswith(('0x', '-0x')) else int(s)


def xreg(r):
    """C expression for reading a general register (64-bit view)."""
    if r in ('xzr', 'wzr'):
        return '0ull'
    if r == 'sp':
        return 'sp'
    if r[0] == 'w':
        return f'(uint64_t)(uint32_t)x{r[1:]}'
    return f'x{r[1:]}'


def wset(r, expr):
    """Assignment to a general register; a w register write zero-extends."""
    if r in ('xzr', 'wzr'):
        return f'(void)({expr});'
    if r == 'sp':
        return f'sp = {expr};'
    if r[0] == 'w':
        return f'x{r[1:]} = (uint64_t)(uint32_t)({expr});'
    return f'x{r[1:]} = {expr};'


def is_w(r):
    return r[0] == 'w'


# FP/SIMD operands: ('s', n) scalar single, ('d', n), ('q', n), ('vec', n, lanes, size), ('el', n, size, idx)
def fpop(s):
    m = re.fullmatch(r'([sdqbh])(\d+)', s)
    if m:
        return (m.group(1), int(m.group(2)))
    m = re.fullmatch(r'v(\d+)\.(\d+)([bhsd])', s)
    if m:
        return ('vec', int(m.group(1)), int(m.group(2)), m.group(3))
    m = re.fullmatch(r'v(\d+)\.([bhsd])\[(\d+)\]', s)
    if m:
        return ('el', int(m.group(1)), m.group(2), int(m.group(3)))
    raise Unsupported(f'operand {s}')


def fp_imm8(word):
    """fmov (scalar, immediate): VFPExpandImm of imm8 for single precision."""
    imm8 = (word >> 13) & 0xFF
    sign = imm8 >> 7
    exp = ((~(imm8 >> 6) & 1) << 7) | (((imm8 >> 6) & 1) * 0x7C) | ((imm8 >> 4) & 3)
    frac = (imm8 & 0xF) << 19
    bits = (sign << 31) | (exp << 23) | frac
    return bits


PT_LOAD, PT_NOTE, NT_GNU_BUILD_ID = 1, 4, 3
EXTERN_KINDS = ('sincosf', 'atan2f', 'abort', 'stack_chk_fail', 'assert')


class Elf:
    """Just enough of a 64-bit arm64 ELF: its load segments, build ID and PLT entries by name."""

    def __init__(self, data):
        self.data = data
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
        self.phdrs = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        names = sections[shstrndx][4]
        self.sections = {data[names + s[0]:data.index(b'\0', names + s[0])].decode(): s for s in sections}

    def segments(self):
        """(vaddr, file offset, file size) of each PT_LOAD."""
        return [(p[3], p[2], p[5]) for p in self.phdrs if p[0] == PT_LOAD]

    def build_id(self):
        for p in self.phdrs:
            if p[0] != PT_NOTE:
                continue
            at, end = p[2], p[2] + p[5]
            while at + 12 <= end:
                namesz, descsz, kind = struct.unpack_from('<III', self.data, at)
                desc = at + 12 + (namesz + 3) // 4 * 4
                if kind == NT_GNU_BUILD_ID:
                    return self.data[desc:desc + descsz]
                at = desc + (descsz + 3) // 4 * 4
        return None

    def imports(self):
        """{imported function name: (its PLT entry's address, its GOT slot's)}. An arm64 .plt has a
        32-byte header, then a 16-byte entry per .rela.plt relocation, in order."""
        plt, rela = self.sections['.plt'], self.sections['.rela.plt']
        dynsym, dynstr = self.sections['.dynsym'], self.sections['.dynstr']
        out = {}
        for i in range(rela[5] // 24):
            slot, info, _addend = struct.unpack_from('<QQq', self.data, rela[4] + 24 * i)
            name_at, = struct.unpack_from('<I', self.data, dynsym[4] + (info >> 32) * 24)
            name = self.data[dynstr[4] + name_at:self.data.index(b'\0', dynstr[4] + name_at)].decode()
            out[name] = (plt[3] + 32 + 16 * i, slot)
        return out

    def plt(self):
        return {name: entry for name, (entry, _slot) in self.imports().items()}


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lifter:
    def __init__(self, data, funcs, externs):
        self.data = data
        self.segments = Elf(data).segments()
        self.funcs = funcs  # addr -> size
        self.externs = externs  # addr -> kind
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.blocks = {}

    def code(self, addr, size):
        for vaddr, off, filesz in self.segments:
            if vaddr <= addr < vaddr + filesz:
                o = off + addr - vaddr
                return self.data[o:o + size]
        raise ValueError(hex(addr))

    def lift(self, start):
        size = self.funcs[start]
        insns = list(self.md.disasm(self.code(start, size), start))
        if sum(i.size for i in insns) != size:
            raise Unsupported(f'{start:#x}: undecodable instruction')
        targets = set()
        for i in insns:
            if i.mnemonic in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or i.mnemonic.startswith('b.'):
                t = imm(split_ops(i.op_str)[-1])
                if not (start <= t < start + size):
                    raise Unsupported(f'{i.address:#x}: branch out of the function')
                targets.add(t)
        body = []
        blocks = set(targets) | {start}
        for i in insns:
            if i.mnemonic.startswith('b.') or i.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz', 'bl'):
                blocks.add(i.address + i.size)
        self.blocks[start] = sorted(b for b in blocks if b < start + size)
        for i in insns:
            if i.address in targets:
                body.append(f'L_{i.address:x}:;')
            if i.address in blocks:
                body.append(f'  COVER({i.address:#x});')
            word, = struct.unpack('<I', i.bytes)
            try:
                c = self.insn(i, word)
            except Unsupported as e:
                raise Unsupported(f'{i.address:#x} {i.mnemonic} {i.op_str}: {e}')
            body.append(f'  /* {i.address:x}: {i.mnemonic} {i.op_str} */ {c}')
        return f'void lift_{start:x}(uint8_t *ts) {{\n  LOAD_ALL;\n' + '\n'.join(body) + '\n}\n'

    # ---- memory operands
    def mem(self, ops, idx):
        """Address of the memory operand at ops[idx]; returns (address expr, writeback stmt or '')."""
        m = ops[idx]
        post = ops[idx + 1] if len(ops) > idx + 1 else None
        wb = m.endswith('!')
        inner = m.rstrip('!')[1:-1]
        parts = [p.strip() for p in inner.split(',')]
        base = xreg(parts[0])
        if len(parts) == 1:
            addr = base
        elif len(parts) == 2:
            addr = f'({base} + (int64_t){imm(parts[1])})'
        elif len(parts) == 3:
            idxr, ext = parts[1], parts[2]
            em = re.fullmatch(r'(sxtw|uxtw|lsl|sxtx)(?: #(\d+))?', ext)
            if not em:
                raise Unsupported(f'extend {ext}')
            sh = int(em.group(2) or 0)
            if em.group(1) == 'sxtw':
                off = f'((uint64_t)(int64_t)(int32_t){xreg(idxr)} << {sh})'
            elif em.group(1) == 'uxtw':
                off = f'((uint64_t)(uint32_t){xreg(idxr)} << {sh})'
            else:
                off = f'({xreg(idxr)} << {sh})'
            addr = f'({base} + {off})'
        else:
            raise Unsupported(f'memory {m}')
        if post is not None:
            return base, wset(parts[0], f'{base} + (int64_t){imm(post)}')
        if wb:
            return addr, wset(parts[0], addr)
        return addr, ''

    # ---- fp helpers
    @staticmethod
    def vname(n):
        return f'v{n}'

    def fp_read(self, op, lane=None):
        """Float expression for a scalar or a vector lane."""
        if op[0] == 's':
            return f'v{op[1]}.s[0]'
        if op[0] == 'el':
            if op[2] != 's':
                raise Unsupported('element size')
            return f'v{op[1]}.s[{op[3]}]'
        if op[0] == 'vec':
            if op[3] != 's':
                raise Unsupported('vector element size')
            return f'v{op[1]}.s[{lane}]'
        raise Unsupported(f'fp operand {op}')

    def fp_write(self, dst, exprs_by_lane):
        """Write lanes into dst: scalar or vector; clears what the instruction clears."""
        n = dst[1]
        tmp = ' '.join(f'float t{k} = {e};' for k, e in enumerate(exprs_by_lane))
        if dst[0] == 's':
            return f'{{ {tmp} v{n} = VZERO; v{n}.s[0] = t0; }}'
        if dst[0] == 'vec':
            lanes = dst[2]
            sets = ' '.join(f'v{n}.s[{k}] = t{k};' for k in range(lanes))
            clear = f'v{n} = VZERO; ' if lanes == 2 else ''
            return f'{{ {tmp} {clear}{sets} }}'
        raise Unsupported(f'fp destination {dst}')

    def lanes(self, dst):
        if dst[0] == 's':
            return 1
        if dst[0] == 'vec' and dst[3] == 's':
            return dst[2]
        raise Unsupported(f'destination {dst}')

    def binop(self, ops, f):
        d, a, b = (fpop(o) for o in ops)
        L = self.lanes(d)
        return self.fp_write(d, [f(self.fp_read(a, k), self.fp_read(b, k)) for k in range(L)])

    # ---- one instruction
    def insn(self, i, word):
        mn, ops = i.mnemonic, split_ops(i.op_str)
        nxt = i.address + i.size
        if mn == 'ret':
            return 'STORE_ALL; return;'
        if mn == 'b':
            return f'goto L_{imm(ops[0]):x};'
        if mn.startswith('b.'):
            return f'if ({CONDS[mn[2:]]}) goto L_{imm(ops[0]):x};'
        if mn in ('cbz', 'cbnz'):
            r = xreg(ops[0])
            r = f'(uint32_t){r}' if is_w(ops[0]) else r
            return f'if ({r} {"==" if mn == "cbz" else "!="} 0) goto L_{imm(ops[1]):x};'
        if mn in ('tbz', 'tbnz'):
            return f'if ((({xreg(ops[0])} >> {imm(ops[1])}) & 1) {"==" if mn == "tbz" else "!="} 0) goto L_{imm(ops[2]):x};'
        if mn == 'bl':
            t = imm(ops[0])
            if t in self.funcs:
                return f'x30 = BASE + {nxt:#x}ull; STORE_ALL; lift_{t:x}(ts); LOAD_ALL;'
            kind = self.externs.get(t)
            if kind == 'sincosf':
                return 'sincosf(v0.s[0], (float *)x0, (float *)x1);'
            if kind == 'atan2f':
                return '{ float r = atan2f(v0.s[0], v1.s[0]); v0 = VZERO; v0.s[0] = r; }'
            if kind == 'assert':
                return 'lift_fatal_assert(x0, x1, x2);'
            if kind in ('abort', 'stack_chk_fail'):
                return f'lift_fatal("{kind}", {i.address:#x});'
            raise Unsupported(f'call to {t:#x}')
        if mn == 'adrp':
            return wset(ops[0], f'BASE + {imm(ops[1]):#x}ull')
        if mn in ('add', 'sub'):
            if len(ops) == 3 and ops[2].startswith('#'):
                v = imm(ops[2])
                e = f'{xreg(ops[1])} {"+" if mn == "add" else "-"} {v:#x}ull'
                return wset(ops[0], e)
            if len(ops) == 4 and ops[2].startswith('#') and ops[3] == 'lsl #12':
                v = imm(ops[2]) << 12
                return wset(ops[0], f'{xreg(ops[1])} {"+" if mn == "add" else "-"} {v:#x}ull')
            if len(ops) == 3:
                return wset(ops[0], f'{xreg(ops[1])} {"+" if mn == "add" else "-"} {xreg(ops[2])}')
            raise Unsupported('form')
        if mn == 'cmp':
            a = xreg(ops[0])
            b = f'{imm(ops[1]):#x}ull' if ops[1].startswith('#') else xreg(ops[1])
            if is_w(ops[0]):
                return f'FLAGS_SUB32((uint32_t){a}, (uint32_t){b});'
            return f'FLAGS_SUB64({a}, {b});'
        if mn == 'cset':
            return wset(ops[0], f'({CONDS[ops[1]]}) ? 1 : 0')
        if mn == 'mov':
            d, s = ops
            if d[0] in 'xw' and d != 'sp' or d in ('sp',):
                if s.startswith('#'):
                    return wset(d, f'{imm(s) & 0xFFFFFFFFFFFFFFFF:#x}ull')
                if s[0] in 'xw' or s in ('sp', 'wzr', 'xzr'):
                    return wset(d, xreg(s))
            # SIMD moves
            if re.fullmatch(r's\d+', d) and '[' in s:
                src = fpop(s)
                return self.fp_write(('s', int(d[1:])), [self.fp_read(src)])
            if d.endswith('.16b') and s.endswith('.16b'):
                return f'v{fpop(d)[1]} = v{fpop(s)[1]};'
            if '[' in d and '[' in s:
                dd, ss = fpop(d), fpop(s)
                if dd[2] != ss[2]:
                    raise Unsupported('element sizes')
                f = {'s': 's', 'd': 'x'}[dd[2]]
                return f'v{dd[1]}.{f}[{dd[3]}] = v{ss[1]}.{f}[{ss[3]}];'
            raise Unsupported('mov form')
        if mn == 'mrs':
            if ops[1] != 'tpidr_el0':
                raise Unsupported('system register')
            return wset(ops[0], f'LD64((uint64_t)(ts + {TLS_OFFSET:#x}))')
        if mn in ('ldr', 'ldur', 'str', 'stur'):
            return self.ldst1(mn, ops)
        if mn in ('ldp', 'stp'):
            return self.ldst2(mn, ops)
        if mn == 'ld1':
            m = re.fullmatch(r'\{v(\d+)\.s\}\[(\d+)\]', ops[0])
            if not m:
                raise Unsupported('ld1 form')
            addr, wb = self.mem(ops, 1)
            return f'v{m.group(1)}.s[{m.group(2)}] = LDF({addr}); {wb}'
        # ---- floating point
        if mn == 'fadd':
            return self.binop(ops, lambda a, b: f'{a} + {b}')
        if mn == 'fsub':
            return self.binop(ops, lambda a, b: f'{a} - {b}')
        if mn == 'fmul':
            return self.binop(ops, lambda a, b: f'{a} * {b}')
        if mn == 'fdiv':
            return self.binop(ops, lambda a, b: f'{a} / {b}')
        if mn == 'fnmul':
            return self.binop(ops, lambda a, b: f'-({a} * {b})')
        if mn in ('fmla', 'fmls'):
            d, a, b = (fpop(o) for o in ops)
            L = self.lanes(d)
            sign = '' if mn == 'fmla' else '-'
            return self.fp_write(d, [f'fmaf({sign}{self.fp_read(a, k)}, {self.fp_read(b, k)}, {self.fp_read(d, k)})' for k in range(L)])
        if mn in ('fmadd', 'fmsub', 'fnmadd', 'fnmsub'):
            d, a, b, c = (fpop(o) for o in ops)
            ra, rb, rc = self.fp_read(a), self.fp_read(b), self.fp_read(c)
            e = {'fmadd': f'fmaf({ra}, {rb}, {rc})', 'fmsub': f'fmaf(-{ra}, {rb}, {rc})',
                 'fnmadd': f'fmaf(-{ra}, {rb}, -{rc})', 'fnmsub': f'fmaf({ra}, {rb}, -{rc})'}[mn]
            return self.fp_write(d, [e])
        if mn in ('fneg', 'fsqrt'):
            d, a = fpop(ops[0]), fpop(ops[1])
            L = self.lanes(d)
            f = (lambda x: f'-{x}') if mn == 'fneg' else (lambda x: f'sqrtf({x})')
            return self.fp_write(d, [f(self.fp_read(a, k)) for k in range(L)])
        if mn == 'faddp':
            d, a = fpop(ops[0]), fpop(ops[1])
            if d[0] != 's' or a[0] != 'vec' or a[2] != 2:
                raise Unsupported('faddp form')
            return self.fp_write(d, [f'v{a[1]}.s[0] + v{a[1]}.s[1]'])
        if mn == 'fmov':
            d = fpop(ops[0])
            if d[0] != 's':
                raise Unsupported('fmov form')
            if ops[1].startswith('#'):
                bits = fp_imm8(word)
                return f'v{d[1]} = VZERO; v{d[1]}.w[0] = {bits:#x}u;'
            return self.fp_write(d, [self.fp_read(fpop(ops[1]))])
        if mn == 'fcmp':
            a = self.fp_read(fpop(ops[0]))
            b = '0.0f' if ops[1].startswith('#') else self.fp_read(fpop(ops[1]))
            return f'FLAGS_FCMP({a}, {b});'
        if mn == 'fccmp':
            a, b = self.fp_read(fpop(ops[0])), self.fp_read(fpop(ops[1]))
            nzcv = imm(ops[2])
            return (f'if ({CONDS[ops[3]]}) {{ FLAGS_FCMP({a}, {b}); }} else {{ n = {nzcv >> 3 & 1}; z = {nzcv >> 2 & 1}; '
                    f'cf = {nzcv >> 1 & 1}; vf = {nzcv & 1}; }}')
        if mn == 'movi':
            d = ops[0]
            if re.fullmatch(r'd\d+', d) or d.endswith('.2d'):
                n = int(re.match(r'[dv](\d+)', d).group(1))
                v = int(ops[1].lstrip('#'), 16)
                if v not in (0, 0xFFFFFFFFFFFFFFFF):
                    raise Unsupported('movi 64-bit mask')
                hi = v if d.endswith('.2d') else 0
                return f'v{n}.x[0] = {v:#x}ull; v{n}.x[1] = {hi:#x}ull;'
            dv = fpop(d)
            if dv[0] != 'vec' or dv[3] != 's':
                raise Unsupported('movi form')
            v = imm(ops[1])
            if len(ops) == 3:
                m = re.fullmatch(r'lsl #(\d+)', ops[2])
                v <<= int(m.group(1))
            sets = ' '.join(f'v{dv[1]}.w[{k}] = {v:#x}u;' for k in range(dv[2]))
            return f'v{dv[1]} = VZERO; {sets}'
        if mn == 'dup':
            d, s = fpop(ops[0]), fpop(ops[1])
            if s[0] != 'el':
                raise Unsupported('dup from general register')
            return self.fp_write(d, [self.fp_read(s)] * self.lanes(d))
        if mn in ('zip1', 'zip2', 'trn1', 'trn2', 'rev64', 'ext'):
            return self.permute(mn, ops)
        raise Unsupported('instruction')

    def permute(self, mn, ops):
        d = fpop(ops[0])
        n = d[1]
        if mn == 'ext':
            a, b = fpop(ops[1]), fpop(ops[2])
            k = imm(ops[3])
            if d[2:] != (16, 'b'):
                raise Unsupported('ext form')
            return (f'{{ uint8_t t[32]; memcpy(t, v{a[1]}.b, 16); memcpy(t + 16, v{b[1]}.b, 16); '
                    f'memcpy(v{n}.b, t + {k}, 16); }}')
        if d[0] != 'vec' or d[3] != 's':
            raise Unsupported('permute size')
        L = d[2]
        if mn == 'rev64':
            a = fpop(ops[1])
            src = [f'v{a[1]}.w[{k ^ 1}]' for k in range(L)]
        else:
            a, b = fpop(ops[1]), fpop(ops[2])
            A = [f'v{a[1]}.w[{k}]' for k in range(L)]
            B = [f'v{b[1]}.w[{k}]' for k in range(L)]
            h = L // 2
            if mn == 'zip1':
                src = [x for k in range(h) for x in (A[k], B[k])]
            elif mn == 'zip2':
                src = [x for k in range(h, L) for x in (A[k], B[k])]
            elif mn == 'trn1':
                src = [x for k in range(0, L, 2) for x in (A[k], B[k])]
            else:
                src = [x for k in range(1, L, 2) for x in (A[k], B[k])]
        tmp = ' '.join(f'uint32_t t{k} = {e};' for k, e in enumerate(src))
        clear = f'v{n} = VZERO; ' if L == 2 else ''
        sets = ' '.join(f'v{n}.w[{k}] = t{k};' for k in range(L))
        return f'{{ {tmp} {clear}{sets} }}'

    def ldst1(self, mn, ops):
        r = ops[0]
        addr, wb = self.mem(ops, 1)
        load = mn in ('ldr', 'ldur')
        m = re.fullmatch(r'([xwsdq])(\d+)|wzr|xzr', r)
        if not m:
            raise Unsupported('register')
        kind = 'w' if r == 'wzr' else 'x' if r == 'xzr' else m.group(1)
        if kind in 'xw':
            if load:
                return f'{wset(r, f"LD64({addr})" if kind == "x" else f"LD32({addr})")} {wb}'
            val = xreg(r)
            return f'{"ST64" if kind == "x" else "ST32"}({addr}, {val}); {wb}'
        n = int(m.group(2))
        if load:
            if kind == 's':
                return f'{{ uint64_t a_ = {addr}; v{n} = VZERO; v{n}.w[0] = LD32(a_); }} {wb}'
            if kind == 'd':
                return f'{{ uint64_t a_ = {addr}; v{n} = VZERO; v{n}.x[0] = LD64(a_); }} {wb}'
            return f'LDQ(&v{n}, {addr}); {wb}'
        if kind == 's':
            return f'ST32({addr}, v{n}.w[0]); {wb}'
        if kind == 'd':
            return f'ST64({addr}, v{n}.x[0]); {wb}'
        return f'STQ({addr}, &v{n}); {wb}'

    def ldst2(self, mn, ops):
        r1, r2 = ops[0], ops[1]
        addr, wb = self.mem(ops, 2)
        load = mn == 'ldp'
        k1 = re.fullmatch(r'([xwsdq])(\d+)|wzr|xzr', r1)
        kind = k1.group(1) if k1.group(1) else ('w' if r1 == 'wzr' else 'x')
        size = {'x': 8, 'w': 4, 's': 4, 'd': 8, 'q': 16}[kind]
        out = [f'{{ uint64_t a_ = {addr};']
        for j, r in enumerate((r1, r2)):
            a = f'a_ + {j * size}'
            if kind in 'xw':
                if load:
                    out.append(wset(r, f'LD64({a})' if kind == 'x' else f'LD32({a})'))
                else:
                    out.append(f'{"ST64" if kind == "x" else "ST32"}({a}, {xreg(r)});')
            else:
                n = int(r[1:])
                if load:
                    if kind == 's':
                        out.append(f'v{n} = VZERO; v{n}.w[0] = LD32({a});')
                    elif kind == 'd':
                        out.append(f'v{n} = VZERO; v{n}.x[0] = LD64({a});')
                    else:
                        out.append(f'LDQ(&v{n}, {a});')
                else:
                    if kind == 's':
                        out.append(f'ST32({a}, v{n}.w[0]);')
                    elif kind == 'd':
                        out.append(f'ST64({a}, v{n}.x[0]);')
                    else:
                        out.append(f'STQ({a}, &v{n});')
        out.append('}')  # the address is taken once, before either load: ldp x0, x1, [x0] is fine
        return ' '.join(out) + (' ' + wb if wb else '')


PRELUDE = r'''#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <string.h>

// Built for FMA, function by function (arm64's fused multiply-adds are fmaf, an instruction only
// with it): nothing here may run before the CPU is known to have it.
#pragma clang attribute push(__attribute__((target("fma"))), apply_to = function)

typedef union { float s[4]; double d[2]; uint32_t w[4]; uint64_t x[2]; uint8_t b[16]; } vreg;
static const vreg VZERO;
#ifndef COVER
#define COVER(a)
#endif
#ifndef LD32
static inline uint32_t LD32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)a, 4); return v; }
static inline uint64_t LD64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }
static inline float LDF(uint64_t a) { float v; memcpy(&v, (const void *)a, 4); return v; }
static inline void LDQ(vreg *r, uint64_t a) { memcpy(r, (const void *)a, 16); }
static inline void ST32(uint64_t a, uint32_t v) { memcpy((void *)a, &v, 4); }
static inline void ST64(uint64_t a, uint64_t v) { memcpy((void *)a, &v, 8); }
static inline void STQ(uint64_t a, const vreg *r) { memcpy((void *)a, r, 16); }
#endif
#define FLAGS_SUB32(a, b) do { uint32_t a_ = (a), b_ = (b), r_ = a_ - b_; n = (int32_t)r_ < 0; z = r_ == 0; \
    cf = a_ >= b_; vf = (((a_ ^ b_) & (a_ ^ r_)) >> 31) & 1; } while (0)
#define FLAGS_SUB64(a, b) do { uint64_t a_ = (a), b_ = (b), r_ = a_ - b_; n = (int64_t)r_ < 0; z = r_ == 0; \
    cf = a_ >= b_; vf = (((a_ ^ b_) & (a_ ^ r_)) >> 63) & 1; } while (0)
#define FLAGS_FCMP(a, b) do { float a_ = (a), b_ = (b); \
    if (a_ != a_ || b_ != b_) { n = 0; z = 0; cf = 1; vf = 1; } \
    else if (a_ == b_) { n = 0; z = 1; cf = 1; vf = 0; } \
    else if (a_ < b_) { n = 1; z = 0; cf = 0; vf = 0; } \
    else { n = 0; z = 0; cf = 1; vf = 0; } } while (0)

#define X_(n) (*(uint64_t *)(ts + 8 * (n)))
#define SP_ (*(uint64_t *)(ts + 0x108))
#define V_(n) (*(vreg *)(ts + 0x110 + 16 * (n)))
#define LOAD_ALL do { REGS(LOAD1) sp = SP_; n = z = cf = vf = 0; } while (0)
#define STORE_ALL do { REGS(STORE1) SP_ = sp; } while (0)
#define LOAD1(t, r, i) r = t##_(i);
#define STORE1(t, r, i) t##_(i) = r;
#define REGS(F) F(X, x0, 0) F(X, x1, 1) F(X, x2, 2) F(X, x3, 3) F(X, x4, 4) F(X, x5, 5) F(X, x6, 6) F(X, x7, 7) \
    F(X, x8, 8) F(X, x9, 9) F(X, x10, 10) F(X, x11, 11) F(X, x12, 12) F(X, x13, 13) F(X, x14, 14) F(X, x15, 15) \
    F(X, x16, 16) F(X, x17, 17) F(X, x18, 18) F(X, x19, 19) F(X, x20, 20) F(X, x21, 21) F(X, x22, 22) F(X, x23, 23) \
    F(X, x24, 24) F(X, x25, 25) F(X, x26, 26) F(X, x27, 27) F(X, x28, 28) F(X, x29, 29) F(X, x30, 30) \
    F(V, v0, 0) F(V, v1, 1) F(V, v2, 2) F(V, v3, 3) F(V, v4, 4) F(V, v5, 5) F(V, v6, 6) F(V, v7, 7) \
    F(V, v8, 8) F(V, v9, 9) F(V, v10, 10) F(V, v11, 11) F(V, v12, 12) F(V, v13, 13) F(V, v14, 14) F(V, v15, 15) \
    F(V, v16, 16) F(V, v17, 17) F(V, v18, 18) F(V, v19, 19) F(V, v20, 20) F(V, v21, 21) F(V, v22, 22) F(V, v23, 23) \
    F(V, v24, 24) F(V, v25, 25) F(V, v26, 26) F(V, v27, 27) F(V, v28, 28) F(V, v29, 29) F(V, v30, 30) F(V, v31, 31)
#define DECL_ALL uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, \
    x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, sp; \
    vreg v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, \
    v22, v23, v24, v25, v26, v27, v28, v29, v30, v31; int n, z, cf, vf;

extern uint64_t BASE;  // the library's load address
void lift_fatal_assert(uint64_t file, uint64_t format, uint64_t arg);
void lift_fatal(const char *what, uint64_t at);
'''


TABLE = '''
struct prism_lifted {
  uint32_t offset, size;  // the guest function: its address from the library's base, its length
  uint64_t fnv;           // FNV-1a of its bytes
  void (*lifted)(uint8_t *);
  const char *name;
};
'''


def lift_library(data, functions, externs, source=''):
    """C source for the functions ({address: (size, description)}) of the library in data;
    externs is {imported name or address: kind} (EXTERN_KINDS). Returns (source, blocks): blocks
    is {function: [basic block addresses]}, for coverage checks."""
    elf = Elf(data)
    plt = elf.plt()
    by_addr = {}
    for key, kind in externs.items():
        if kind not in EXTERN_KINDS:
            raise ValueError(f'extern kind {kind}')
        if isinstance(key, str):
            if key not in plt:
                raise Unsupported(f'{key}: not imported')
            by_addr[plt[key]] = kind
        else:
            by_addr[key] = kind
    build_id = elf.build_id()
    if not build_id:
        raise Unsupported('no GNU build ID')
    lifter = Lifter(data, {a: size for a, (size, _) in functions.items()}, by_addr)
    parts = [f'// Generated by tools/lift_arm64.py from {source} (build ID {build_id.hex()}); do not edit.', PRELUDE]
    parts += [f'void lift_{a:x}(uint8_t *ts);' for a in functions]
    for a in functions:
        parts.append(lifter.lift(a).replace('LOAD_ALL;\n', 'DECL_ALL\n  LOAD_ALL;\n', 1))
    parts.append('#pragma clang attribute pop')
    parts.append(TABLE)
    parts.append('const struct prism_lifted prism_lifted[] = {')
    for a, (size, what) in functions.items():
        parts.append(f'    {{{a:#x}, {size}, {fnv1a(lifter.code(a, size)):#018x}ull, lift_{a:x}, "lift_{a:x}"}},  // {what}')
    parts.append('};')
    parts.append(f'const unsigned prism_lifted_count = {len(functions)};')
    parts.append(f'const uint8_t prism_lifted_build_id[{len(build_id)}] = {{{", ".join(f"{b:#04x}" for b in build_id)}}};')
    parts.append(f'const unsigned prism_lifted_build_id_size = {len(build_id)};\n')
    return '\n'.join(parts), lifter.blocks


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    lib, out = sys.argv[1], sys.argv[2]
    functions, externs = {}, {}
    mode = functions
    for a in sys.argv[3:]:
        if a == '--extern':
            mode = externs
            continue
        k, v = a.split('=')
        if mode is functions:
            functions[int(k, 16)] = (int(v, 0), '')
        else:
            externs[int(k, 16) if re.fullmatch(r'(0x)?[0-9a-fA-F]+', k) and not k.isalpha() else k] = v
    with open(lib, 'rb') as f:
        source, blocks = lift_library(f.read(), functions, externs, lib)
    with open(out, 'w', newline='\n') as f:
        f.write(source)
    with open(out + '.blocks', 'w') as f:
        for bl in blocks.values():
            f.write(' '.join(f'{b:x}' for b in bl) + '\n')


if __name__ == '__main__':
    main()
