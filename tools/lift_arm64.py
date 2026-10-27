"""Arm64 functions of a guest library as C for x86_64: Horizon code that runs native instead of
translated (native/berberis_proxy_libshell).

    python tools/lift_arm64.py LIB OUT.c ADDR[=SIZE]... [--extern NAME=KIND...] [--extern ADDR=KIND...]

Digitalis keeps a guest's registers in memory and checks its float math instruction by instruction,
so hot code such as VrShell's ray hit-testing and transform math runs many times slower translated
than compiled. This turns each listed function, instruction by instruction, into a C function on
locals that a compiler keeps in registers:

  * `void lift_<addr>(uint8_t *ts)`, ts the translator's ThreadState (CPUState first: x0-x30 at 0,
    sp at 0x108, v0-v31 at 0x110; the guest TLS pointer, which mrs tpidr_el0 reads, at 0x358). It
    reads every register at entry and writes every one back at its ret, so it does what the guest
    function does to the registers, its stack (the guest's: sp) and memory, as the translator's
    trampolines expect of a host function standing in for guest code (they then return to x30).
  * A function is the code reachable from its address: its own, and any other it branches into
    (a shared tail, a cold part, a tail call), so its eh_frame size doesn't matter.
  * Calls to other listed functions are C calls, the registers going through ts. Calls to externs
    are host calls (libm, as Prism's libm proxy makes the guest's; memcpy, memmove, memset) or fatal
    (an assert's report, abort, the stack check). Any other call (bl, blr; a branch to an import)
    runs the guest code translated: lift_guest_call (the proxy's runtime) has the translator run
    it on ts and return.
  * Integer semantics are arm64's (flags, shifts, bitfields, division by zero, saturating float to
    integer conversions); acquire/release accesses are sequentially consistent atomics, an
    exclusive pair a compare-and-swap on the value the load saw.
  * Float semantics are arm64's: fmadd, fmla and the like are fused (fmaf, so the lifted functions
    are built for FMA, and the runtime checks the CPU has it before using them), writes to s
    registers clear the rest of the vector register, 64-bit vector operations clear its upper half.
    One known difference: a NaN an operation creates from non-NaN operands (0 * inf) is x86's
    default NaN, 0xffc00000, where arm64's is 0x7fc00000 (the sign bit; comparisons don't see it).
  * Indirect jumps (br), system instructions and instructions it doesn't know are refused
    (Unsupported), so a function is lifted whole or not at all.

The output also has the table native/berberis_proxy_libshell's runtime checks before using any of
it: each function's offset, the address ranges of its code, an FNV-1a hash of those bytes, and the
library's GNU build ID. Each basic block starts with COVER(addr), empty unless the includer defines
it, and the memory accesses go through LD8...STQ, which it may also define (LIFT_MEMORY):
native/berberis_proxy_libshell/check records and replays calls through them. Needs capstone (as
tools/reverse.py does).
"""
import bisect
import re
import struct
import sys

import capstone

TLS_OFFSET = 0x358  # ThreadState::tls
MAX_INSNS = 20000   # a function's reachable code, at most
CONDS = {
    'eq': 'z', 'ne': '!z', 'hs': 'cf', 'cs': 'cf', 'lo': '!cf', 'cc': '!cf', 'mi': 'n', 'pl': '!n',
    'vs': 'vf', 'vc': '!vf', 'hi': 'cf && !z', 'ls': '!cf || z', 'ge': 'n == vf', 'lt': 'n != vf',
    'gt': '!z && n == vf', 'le': 'z || n != vf', 'al': '1', 'nv': '1',
}
BRANCHES = ('b', 'cbz', 'cbnz', 'tbz', 'tbnz')
ENDS = ('b', 'ret', 'br', 'brk', 'udf', 'hlt')  # no fall-through
NORETURN = ('assert', 'abort', 'stack_chk_fail')


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


def simm(s):
    """An address offset, which capstone sometimes prints as its 64-bit two's complement."""
    v = imm(s)
    return v - (1 << 64) if v >= 1 << 63 else v


GPR = re.compile(r'[xw]\d+|[xw]zr|sp')


def is_gpr(s):
    return bool(GPR.fullmatch(s))


def xreg(r):
    """C expression for reading a general register (64-bit view)."""
    if r in ('xzr', 'wzr'):
        return '0ull'
    if r == 'sp':
        return 'sp'
    if not is_gpr(r):
        raise Unsupported(f'register {r}')
    if r[0] == 'w':
        return f'(uint64_t)(uint32_t)x{r[1:]}'
    return f'x{r[1:]}'


def wset(r, expr):
    """Assignment to a general register; a w register write zero-extends."""
    if r in ('xzr', 'wzr'):
        return f'(void)({expr});'
    if r == 'sp':
        return f'sp = {expr};'
    if not is_gpr(r):
        raise Unsupported(f'register {r}')
    if r[0] == 'w':
        return f'x{r[1:]} = (uint64_t)(uint32_t)({expr});'
    return f'x{r[1:]} = {expr};'


def is_w(r):
    return r[0] == 'w'


def width(r):
    return 32 if r[0] == 'w' else 64


def ut(W):
    return {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}[W]


def st(W):
    return {8: 'int8_t', 16: 'int16_t', 32: 'int32_t', 64: 'int64_t'}[W]


def mask(W):
    return (1 << W) - 1


def rd(r, W):
    """Register r as a W-bit unsigned value."""
    if r in ('xzr', 'wzr'):
        return f'({ut(W)})0'
    if r == 'sp':
        if W != 64:
            raise Unsupported('32-bit sp')
        return 'sp'
    if not is_gpr(r):
        raise Unsupported(f'register {r}')
    return f'({ut(W)})x{r[1:]}'


def konst(v, W):
    return f'({ut(W)}){v & mask(W):#x}ull'


def flags_nz(r, W):
    return f'n = ({r}) >> {W - 1}; z = ({r}) == 0;'


def arith(a, b, W, sub, result=None):
    """a + b or a - b in W bits, setting the flags as adds/subs do, then `result(expr)`'s
    statement."""
    t = ut(W)
    op = '-' if sub else '+'
    code = f'{{ {t} fa_ = {a}, fb_ = {b}, fr_ = ({t})(fa_ {op} fb_); {flags_nz("fr_", W)}'
    if sub:
        code += f' cf = fa_ >= fb_; vf = (((fa_ ^ fb_) & (fa_ ^ fr_)) >> {W - 1}) & 1;'
    else:
        code += f' cf = fr_ < fa_; vf = ((~(fa_ ^ fb_) & (fa_ ^ fr_)) >> {W - 1}) & 1;'
    if result:
        code += ' ' + result('fr_')
    return code + ' }'


# FP/SIMD operands: ('s', n) scalar single, ('d', n), ('q', n), ('h', n), ('b', n),
# ('vec', n, lanes, size), ('el', n, size, idx)
def fpop(s):
    m = re.fullmatch(r'([sdqbh])(\d+)', s)
    if m:
        return (m.group(1), int(m.group(2)))
    m = re.fullmatch(r'v(\d+)\.(\d+)([bhsd])', s)
    if m:
        return ('vec', int(m.group(1)), int(m.group(2)), m.group(3))
    m = re.fullmatch(r'v(\d+)\.([bhsd])\[(\d+|0x[0-9a-f]+)\]', s)
    if m:
        return ('el', int(m.group(1)), m.group(2), int(m.group(3), 0))
    raise Unsupported(f'operand {s}')


BITS = {'b': 8, 'h': 16, 's': 32, 'd': 64, 'q': 128}
UFIELD = {'b': 'b', 'h': 'h', 's': 'w', 'd': 'x'}  # vreg's unsigned lanes by element size
FFIELD = {'s': 's', 'd': 'd'}                      # and its float lanes
CTYPE = {'b': 'uint8_t', 'h': 'uint16_t', 'w': 'uint32_t', 'x': 'uint64_t', 's': 'float', 'd': 'double'}


def esize(op):
    """Element size letter of an FP/SIMD operand."""
    if op[0] == 'vec':
        return op[3]
    if op[0] == 'el':
        return op[2]
    return op[0]


def nlanes(op):
    if op[0] == 'vec':
        return op[2]
    if op[0] == 'q':
        raise Unsupported('q as scalar')
    return 1


def lane(op, k, kind='f'):
    """Lane k of an operand (an element operand is the same lane whatever k): kind 'f' float,
    'u' unsigned, 'i' signed."""
    if op[0] == 'vec':
        n, es, idx = op[1], op[3], k
    elif op[0] == 'el':
        n, es, idx = op[1], op[2], op[3]
    elif op[0] in 'bhsd':
        n, es, idx = op[1], op[0], 0
    else:
        raise Unsupported(f'operand {op}')
    if kind == 'f':
        if es not in FFIELD:
            raise Unsupported('half-precision')
        return f'v{n}.{FFIELD[es]}[{idx}]'
    e = f'v{n}.{UFIELD[es]}[{idx}]'
    return f'({st(BITS[es])}){e}' if kind == 'i' else e


def put(dst, exprs, kind='f'):
    """Write lanes into an FP/SIMD destination, clearing what the instruction clears (all but a
    scalar, the upper half after a 64-bit vector)."""
    n, es = dst[1], esize(dst)
    if dst[0] not in ('vec', 'b', 'h', 's', 'd'):
        raise Unsupported(f'destination {dst}')
    field = FFIELD[es] if kind == 'f' else UFIELD[es]
    ctype = CTYPE[field]
    tmp = ' '.join(f'{ctype} t{k} = {e};' for k, e in enumerate(exprs))
    clear = f'v{n} = VZERO; ' if len(exprs) * BITS[es] < 128 else ''
    sets = ' '.join(f'v{n}.{field}[{k}] = t{k};' for k in range(len(exprs)))
    return f'{{ {tmp} {clear}{sets} }}'


def fp_imm8(word, double=False, vector=False):
    """fmov (immediate): VFPExpandImm of imm8 for single or double precision; the bits. The
    scalar form has imm8 at bits 20-13, the vector one as AdvSIMD's a:b:c (18-16), d:e:f:g:h (9-5)."""
    imm8 = ((word >> 16) & 7) << 5 | (word >> 5) & 0x1F if vector else (word >> 13) & 0xFF
    sign, b6 = imm8 >> 7, (imm8 >> 6) & 1
    if double:
        exp = ((b6 ^ 1) << 10) | (b6 * 0xFF << 2) | ((imm8 >> 4) & 3)
        return (sign << 63) | (exp << 52) | ((imm8 & 0xF) << 48)
    exp = ((b6 ^ 1) << 7) | (b6 * 0x1F << 2) | ((imm8 >> 4) & 3)
    return (sign << 31) | (exp << 23) | ((imm8 & 0xF) << 19)


def adv_simd_imm(word):
    """The 64-bit value of an AdvSIMD modified immediate (movi, mvni, orr, bic: op and cmode)."""
    abc, defgh = (word >> 16) & 7, (word >> 5) & 0x1F
    imm8 = (abc << 5) | defgh
    cmode, op = (word >> 12) & 0xF, (word >> 29) & 1
    if cmode < 8:
        v32 = imm8 << (8 * (cmode >> 1))
        return v32 | v32 << 32
    if cmode < 12:
        v16 = imm8 << (8 * ((cmode >> 1) & 1))
        return v16 * 0x0001000100010001
    if cmode < 14:
        v32 = (imm8 << 8 | 0xFF) if cmode & 1 == 0 else (imm8 << 16 | 0xFFFF)
        return v32 | v32 << 32
    if cmode == 14 and op == 0:
        return imm8 * 0x0101010101010101
    if cmode == 14:
        return sum(0xFF << (8 * k) for k in range(8) if imm8 >> k & 1)
    raise Unsupported('fmov vector immediate here')


# Narrow loads and stores: mnemonic -> (load, bytes, sign-extends). A w destination of a signed load
# keeps 32 bits of the extension (wset zero-extends from there), as arm64 does.
NARROW = {
    'ldrb': (True, 1, False), 'ldurb': (True, 1, False), 'ldrh': (True, 2, False), 'ldurh': (True, 2, False),
    'ldrsb': (True, 1, True), 'ldursb': (True, 1, True), 'ldrsh': (True, 2, True), 'ldursh': (True, 2, True),
    'ldrsw': (True, 4, True), 'ldursw': (True, 4, True),
    'strb': (False, 1, False), 'sturb': (False, 1, False), 'strh': (False, 2, False), 'sturh': (False, 2, False),
}
# Acquire loads and release stores: mnemonic -> (load, bytes or 0 for the register's size).
ORDERED = {
    'ldar': (True, 0), 'ldarb': (True, 1), 'ldarh': (True, 2), 'ldapr': (True, 0), 'ldaprb': (True, 1),
    'ldaprh': (True, 2), 'ldapur': (True, 0), 'ldapurb': (True, 1), 'ldapurh': (True, 2),
    'stlr': (False, 0), 'stlrb': (False, 1), 'stlrh': (False, 2), 'stlur': (False, 0), 'stlurb': (False, 1),
    'stlurh': (False, 2),
}
# Exclusive loads and stores (a CAS on the value the load saw): mnemonic -> (load, bytes or 0).
EXCLUSIVE = {
    'ldxr': (True, 0), 'ldaxr': (True, 0), 'ldxrb': (True, 1), 'ldaxrb': (True, 1), 'ldxrh': (True, 2),
    'ldaxrh': (True, 2), 'stxr': (False, 0), 'stlxr': (False, 0), 'stxrb': (False, 1), 'stlxrb': (False, 1),
    'stxrh': (False, 2), 'stlxrh': (False, 2),
}
EXCLUSIVE_PAIRS = ('ldxp', 'ldaxp', 'stxp', 'stlxp')  # 64-bit pairs: a 16-byte CAS (cmpxchg16b)
# Atomic read-modify-writes (LSE): the new value from the old, o_, and the operand, v_.
LSE = {'add': 'o_ + v_', 'clr': 'o_ & ~v_', 'eor': 'o_ ^ v_', 'set': 'o_ | v_', 'swp': 'v_'}
ATOMIC = re.compile(r'(?:(ld|st)(add|clr|eor|set)|(swp)|(cas))(a|l|al)?([bh])?')

PT_LOAD, PT_NOTE, NT_GNU_BUILD_ID = 1, 4, 3
# Host calls the lifted code makes in place of a guest library's: float(float) and
# float(float, float) libm functions, sincosf, the memory functions, and fatal ones; 'guest' makes
# a call or branch to the address a guest call.
LIBM1 = ('sinf', 'cosf', 'tanf', 'asinf', 'acosf', 'atanf', 'expf', 'exp2f', 'logf', 'log2f', 'log10f',
         'sqrtf', 'floorf', 'ceilf', 'roundf', 'truncf', 'fabsf', 'sinhf', 'coshf', 'tanhf', 'cbrtf')
LIBM2 = ('atan2f', 'powf', 'fmodf', 'hypotf', 'fminf', 'fmaxf')
EXTERN_KINDS = ('sincosf', 'abort', 'stack_chk_fail', 'assert', 'memcpy', 'memmove', 'memset', 'guest') + LIBM1 + LIBM2


class Elf:
    """Just enough of a 64-bit arm64 ELF: its load segments, build ID, PLT entries by name and
    function ranges (.eh_frame_hdr)."""

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

    def plt_range(self):
        plt = self.sections['.plt']
        return plt[3], plt[3] + plt[5]

    def functions(self):
        """{start: size} of every function .eh_frame describes (its .eh_frame_hdr's table)."""
        hdr, eh = self.sections['.eh_frame_hdr'], self.sections['.eh_frame']
        count, = struct.unpack_from('<I', self.data, hdr[4] + 8)
        out = {}
        for i in range(count):
            loc, fde = struct.unpack_from('<ii', self.data, hdr[4] + 12 + 8 * i)
            fde_at = eh[4] + (hdr[3] + fde - eh[3])
            out[hdr[3] + loc], = struct.unpack_from('<I', self.data, fde_at + 12)
        return out


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


class Lifter:
    def __init__(self, data, funcs, externs):
        self.data = data
        elf = Elf(data)
        self.segments = elf.segments()
        self.plt_lo, self.plt_hi = elf.plt_range() if '.plt' in elf.sections else (0, 0)
        self.starts = set(elf.functions()) if '.eh_frame_hdr' in elf.sections else set()
        self._sorted_starts = sorted(self.starts)
        self.funcs = funcs  # the lifted functions' addresses (a C call each)
        self.externs = externs  # addr -> kind
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.mdd = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.mdd.detail = True  # for the registers an instruction writes
        self.blocks = {}
        self.jumps = {}  # a jump table's br -> (its bound check's b.hi, its targets)
        self.ranges = {}  # function -> [(address, size)] of its code
        self.guest_calls = {}  # function -> number of call sites that run guest code
        self.stops = {}  # function -> its guest calls' return addresses, where the translator stops
        self._sources = None  # branch target -> the branches to it, for foreign()
        self._own = set()  # the function being lifted: its instructions
        self._tainted = set()  # ... and those the translator may run (translated_reach)

    def code(self, addr, size):
        for vaddr, off, filesz in self.segments:
            if vaddr <= addr < vaddr + filesz:
                o = off + addr - vaddr
                return self.data[o:o + size]
        raise Unsupported(f'{addr:#x}: not in the library')

    def decode(self, addr):
        insns = list(self.md.disasm(self.code(addr, 4), addr))
        if not insns:
            raise Unsupported(f'{addr:#x}: undecodable instruction')
        return insns[0]

    def branch_sources(self):
        """{target: [addresses of the direct branches to it]} over the library's .text (numpy)."""
        if self._sources is None:
            import numpy as np
            elf = Elf(self.data)
            text = elf.sections['.text']
            w = np.frombuffer(self.data, dtype='<u4', count=text[5] // 4, offset=text[4]).astype(np.int64)
            pc = text[3] + 4 * np.arange(len(w), dtype=np.int64)
            def sext(v, bits):
                return np.where(v >= 1 << (bits - 1), v - (1 << bits), v)
            targets = np.full(len(w), -1, dtype=np.int64)
            b = (w & 0xFC000000) == 0x14000000
            targets[b] = pc[b] + 4 * sext(w[b] & 0x3FFFFFF, 26)
            c = ((w & 0xFF000010) == 0x54000000) | ((w & 0x7E000000) == 0x34000000)
            targets[c] = pc[c] + 4 * sext((w[c] >> 5) & 0x7FFFF, 19)
            t = (w & 0x7E000000) == 0x36000000
            targets[t] = pc[t] + 4 * sext((w[t] >> 5) & 0x3FFF, 14)
            self._sources = {}
            for src, dst in zip(pc[targets >= 0].tolist(), targets[targets >= 0].tolist()):
                self._sources.setdefault(dst, []).append(src)
        return self._sources

    def writes(self, a):
        """The registers (x numbers, and 'nzcv') the instruction at a writes."""
        i = next(self.mdd.disasm(self.code(a, 4), a))
        if i.mnemonic in ('cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'fcmp', 'fcmpe'):
            return {'nzcv'}  # capstone has them write their first operand
        out = set()
        for r in i.regs_access()[1]:
            m = re.fullmatch(r'[xw](\d+)', i.reg_name(r))
            if m:
                out.add(int(m.group(1)))
            elif i.reg_name(r) == 'nzcv':
                out.add('nzcv')
        return out

    def jump_table(self, br):
        """A switch's jump through a table (adr R, base; ldrb/ldrh K, [T, I]; add R, R, K, lsl #2;
        br R, with T from adrp+add and I checked by cmp I, #N; b.hi before them): (the b.hi's
        address, the targets: base + 4 * entry for the N + 1 entries, all in br's function). None
        if br is something else (an indirect tail call); Unsupported if it computes its target
        but isn't this."""
        if br in self.jumps:
            return self.jumps[br]
        r = int(self.decode(br).op_str[1:])
        found, a = {}, br - 4
        while a >= br - 4 * 24 and 'cmp' not in found:
            i = self.decode(a)
            mn, ops = i.mnemonic, split_ops(i.op_str)
            if mn in ENDS or mn in BRANCHES or (mn.startswith('b.') and mn != 'b.hi'):
                break
            w = self.writes(a)
            if 'add' not in found:
                if r in w:
                    m = re.fullmatch(r'x(\d+), x(\d+), x(\d+), lsl #2', i.op_str)
                    if mn != 'add':
                        return None
                    if not m or int(m.group(1)) != r or int(m.group(2)) != r:
                        self._no_jump(br, 'computed target')
                    found['add'] = int(m.group(3))
            elif 'adr' not in found and r in w:
                if mn != 'adr':
                    self._no_jump(br, 'table base')
                found['adr'] = imm(ops[1])
            elif 'ldr' not in found and found['add'] in w:
                m = re.fullmatch(r'w\d+, \[x(\d+), x(\d+)(, lsl #1)?\]', i.op_str)
                if mn not in ('ldrb', 'ldrh') or not m or (mn == 'ldrh') != bool(m.group(3)):
                    self._no_jump(br, 'table load')
                found['ldr'] = (mn, int(m.group(1)), int(m.group(2)))
            elif 'ldr' in found and 'tadd' not in found and found['ldr'][1] in w:
                m = re.fullmatch(r'x(\d+), x(\d+), #(\w+)', i.op_str)
                if mn != 'add' or not m or int(m.group(2)) != found['ldr'][1]:
                    self._no_jump(br, 'table address')
                found['tadd'] = imm(m.group(3))
            elif 'tadd' in found and 'adrp' not in found and found['ldr'][1] in w:
                if mn != 'adrp':
                    self._no_jump(br, 'table address')
                found['adrp'] = imm(ops[1])
            elif 'ldr' in found and found['ldr'][2] in w:
                self._no_jump(br, 'index changed after its check')
            elif mn == 'b.hi' and 'ldr' in found and 'bhi' not in found:
                found['bhi'] = a
            elif mn == 'cmp' and 'bhi' in found:
                m = re.fullmatch(r'[xw](\d+), #(\w+)', i.op_str)
                if not m or int(m.group(1)) != found['ldr'][2]:
                    self._no_jump(br, 'bound check')
                found['cmp'] = imm(m.group(2))
            elif 'bhi' in found and 'nzcv' in w:
                self._no_jump(br, 'flags set after the bound check')
            a -= 4
        if 'add' not in found:
            return None
        k = bisect.bisect_right(self._sorted_starts, br) - 1
        lo = self._sorted_starts[k] if k >= 0 else 0
        hi = self._sorted_starts[k + 1] if k + 1 < len(self._sorted_starts) else 1 << 64
        if {'adr', 'ldr', 'bhi', 'cmp'} <= set(found) and 'adrp' not in found and 19 <= found['ldr'][1] <= 28:
            found.update(self.table_register(found['ldr'][1], lo, hi))
        if not {'adr', 'ldr', 'tadd', 'adrp', 'bhi', 'cmp'} <= set(found):
            self._no_jump(br, 'unrecognized')
        size = 1 if found['ldr'][0] == 'ldrb' else 2
        table = found['adrp'] + found['tadd']
        out = []
        for n in range(found['cmp'] + 1):
            t = found['adr'] + 4 * int.from_bytes(self.code(table + n * size, size), 'little')
            if not lo <= t < hi:
                self._no_jump(br, f'entry {n} leaves the function')
            out.append(t)
        self.jumps[br] = (found['bhi'], sorted(set(out)))
        return self.jumps[br]

    def table_register(self, t, lo, hi):
        """A jump table's address in callee-saved x<t>, set once for the function [lo, hi): every
        instruction there writing it is the same adrp and add, or its epilogue's restore.
        {'adrp': page, 'tadd': offset}, or {} if not. (The jump goes where the code computes all
        the same: this only finds the targets to lift.)"""
        pages, offsets = set(), set()
        for a in range(lo, min(hi, lo + 4 * 20000), 4):
            try:
                if t not in self.writes(a):
                    continue
            except (StopIteration, Unsupported):
                continue
            i = self.decode(a)
            ops = split_ops(i.op_str)
            if i.mnemonic == 'adrp':
                pages.add(imm(ops[1]))
            elif i.mnemonic == 'add' and re.fullmatch(rf'x{t}, x{t}, #\w+', i.op_str):
                offsets.add(imm(ops[2]))
            elif i.mnemonic in ('ldp', 'ldr') and '[sp' in i.op_str:
                pass  # the epilogue's restore
            else:
                return {}
        if len(pages) != 1 or len(offsets) != 1:
            return {}
        return {'adrp': pages.pop(), 'tadd': offsets.pop()}

    def _no_jump(self, br, why):
        raise Unsupported(f'{br:#x}: jump table ({why})')

    def translated_reach(self, start, insns):
        """The instructions of insns the translator may run: those reached from where other code
        enters them (another function's start that isn't lifted, as the code a tail branch here
        follows; a branch from outside; code before them falling in), along fall-throughs and
        insns' own branches. A call there can't return to a stop of its own."""
        entries = set()
        for a in insns:
            if a == start or a in self.funcs:
                # A call there runs it lifted. A branch there from translated code needn't: the
                # translator's optimizing gear follows a b into its target's code (VrShell's
                # 9201d0 is a b to 9201d4), and a call made there would return to a stop here.
                if self.foreign(a):
                    entries.add(a)
                continue
            if a in self.starts:
                entries.add(a)
            elif self.foreign(a):
                entries.add(a)
            elif a != start and a - 4 not in insns and a not in self.starts:
                try:
                    p = self.decode(a - 4)
                except Unsupported:
                    continue
                pops = split_ops(p.op_str)
                if p.mnemonic not in ENDS and not (p.mnemonic == 'bl' and self.externs.get(imm(pops[0])) in NORETURN):
                    entries.add(a)
        seen, todo = set(), list(entries)
        while todo:
            a = todo.pop()
            if a in seen or a not in insns:
                continue
            seen.add(a)
            i = insns[a]
            mn, ops = i.mnemonic, split_ops(i.op_str)
            if mn in BRANCHES or mn.startswith('b.'):
                todo.append(imm(ops[-1]))
            if mn == 'br' and a in self.jumps:
                todo.extend(self.jumps[a][1])
            if mn not in ENDS:
                todo.append(a + 4)
        return seen

    def foreign(self, address):
        """Whether code other than the function being lifted branches to address (then the
        translator may run it, and it can't be a guest call's stop)."""
        return any(src not in self._own for src in self.branch_sources().get(address, ()))

    def tail_target(self, t):
        """Whether a branch to t leaves the function as a guest tail call (to an import's PLT
        entry, an address listed as 'guest'); otherwise t's code is followed as this function's."""
        return self.plt_lo <= t < self.plt_hi or self.externs.get(t) == 'guest'

    def lift(self, start):
        # The reachable code: from the entry, along branches (but those leaving as tail calls).
        insns, todo, targets = {}, [start], {start}
        while todo:
            a = todo.pop()
            if a in insns:
                continue
            if len(insns) >= MAX_INSNS:
                raise Unsupported(f'{start:#x}: more than {MAX_INSNS} instructions reachable')
            i = self.decode(a)
            insns[a] = i
            mn, ops = i.mnemonic, split_ops(i.op_str)
            if mn in BRANCHES or mn.startswith('b.'):
                t = imm(ops[-1])
                if not self.tail_target(t):
                    targets.add(t)
                    todo.append(t)
            if mn == 'br' and self.jump_table(a) is not None:
                for t in self.jumps[a][1]:
                    targets.add(t)
                    todo.append(t)
            noreturn = mn == 'bl' and self.externs.get(imm(ops[0])) in NORETURN
            # Code that would fall into another function is past a call that doesn't return.
            if mn not in ENDS and not noreturn and a + 4 not in self.starts:
                todo.append(a + 4)
        order = sorted(insns)  # a fall-through's next instruction is the next one here
        self._own = set(insns)
        self._tainted = self.translated_reach(start, insns)
        for a, (check, _) in self.jumps.items():
            # Every way to a jump table's br passes its bound check (no branch lands after it).
            if a in insns and any(check < t <= a for t in targets):
                raise Unsupported(f'{a:#x}: a branch past the jump table\'s bound check')
        self.stops[start] = []
        blocks = set(targets)
        for a in order:
            mn = insns[a].mnemonic
            if mn.startswith('b.') or mn in ('cbz', 'cbnz', 'tbz', 'tbnz', 'bl', 'blr'):
                blocks.add(a + 4)
        # Code it branches to may be below its entry (another function's, it tail-branches to).
        body = [] if order[0] == start else [f'  goto L_{start:x};']
        self.guest_calls[start] = 0
        for a in order:
            i = insns[a]
            if a in targets:
                body.append(f'L_{a:x}:;')
            if a in blocks:
                body.append(f'  COVER({a:#x});')
            word, = struct.unpack('<I', i.bytes)
            try:
                c = self.insn(i, word, start)
            except Unsupported as e:
                raise Unsupported(f'{a:#x} {i.mnemonic} {i.op_str}: {e}')
            except (ValueError, KeyError, IndexError, AttributeError, TypeError) as e:
                raise Unsupported(f'{a:#x} {i.mnemonic} {i.op_str}: form ({type(e).__name__} {e})')
            body.append(f'  /* {a:x}: {i.mnemonic} {i.op_str} */ {c}')
            if i.mnemonic not in ENDS and a + 4 not in insns:
                body.append(f'  lift_fatal("returned", {a:#x});')
        self.blocks[start] = sorted(b for b in blocks if b in insns)
        ranges = []
        for a in order:
            if ranges and ranges[-1][0] + ranges[-1][1] == a:
                ranges[-1][1] += 4
            else:
                ranges.append([a, 4])
        self.ranges[start] = [tuple(r) for r in ranges]
        # The registers it names (and x30, and its host calls' arguments and results): only those
        # go between its locals and ts, at its entry, its returns and around its calls. The others
        # keep what ts has, as on arm64 (a callee may change them there).
        xs, vs = {30}, set()
        for i in insns.values():
            xs |= {int(n) for n in re.findall(r'\b[xw](\d+)\b', i.op_str)}
            vs |= {int(a or b) for a, b in re.findall(r'\b[bhsdq](\d+)\b|\bv(\d+)\.', i.op_str)}
            if i.mnemonic == 'bl' and self.externs.get(imm(split_ops(i.op_str)[0])) not in (None, 'guest'):
                xs |= {0, 1, 2}
                vs |= {0, 1}
        load = ' '.join([f'x{n} = X_({n});' for n in sorted(xs)] + [f'v{n} = V_({n});' for n in sorted(vs)])
        store = ' '.join([f'X_({n}) = x{n};' for n in sorted(xs)] + [f'V_({n}) = v{n};' for n in sorted(vs)])
        body = [line.replace('LOAD_ALL;', 'LOAD_F;').replace('STORE_ALL;', 'STORE_F;') for line in body]
        return (f'#define LOAD_F do {{ {load} sp = SP_; n = z = cf = vf = 0; }} while (0)\n'
                f'#define STORE_F do {{ {store} SP_ = sp; }} while (0)\n'
                f'void lift_{start:x}(uint8_t *ts) {{\n  LOAD_F;\n' + '\n'.join(body) + '\n}\n'
                '#undef LOAD_F\n#undef STORE_F\n')

    # ---- memory operands
    def mem(self, ops, idx):
        """Address of the memory operand at ops[idx]; returns (address expr, writeback stmt or '')."""
        m = ops[idx]
        if m.startswith('#'):  # a literal: pc-relative
            return f'(BASE + {imm(m):#x}ull)', ''
        post = ops[idx + 1] if len(ops) > idx + 1 else None
        wb = m.endswith('!')
        inner = m.rstrip('!')[1:-1]
        parts = [p.strip() for p in inner.split(',')]
        base = xreg(parts[0])
        if len(parts) == 1:
            addr = base
        elif len(parts) == 2 and parts[1].startswith('#'):
            addr = f'({base} + (int64_t){simm(parts[1])})'
        elif len(parts) == 2:
            addr = f'({base} + {xreg(parts[1])})'
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
            step = f'(int64_t){simm(post)}' if post.startswith('#') else xreg(post)
            return base, wset(parts[0], f'{base} + {step}')
        if wb:
            return addr, wset(parts[0], addr)
        return addr, ''

    # ---- data-processing operands
    def operand2(self, ops, W):
        """C expression (W bits, unsigned) of a data-processing instruction's last operand: an
        immediate (shifted), a register, shifted or extended."""
        first = ops[0]
        mod = ops[1] if len(ops) > 1 else None
        if len(ops) > 2:
            raise Unsupported('operand form')
        if first.startswith('#'):
            v = imm(first)
            if mod:
                m = re.fullmatch(r'lsl #(\w+)', mod)
                if not m:
                    raise Unsupported(f'immediate shift {mod}')
                v <<= imm(m.group(1))
            return konst(v, W)
        t = ut(W)
        if mod is None:
            return rd(first, W)
        m = re.fullmatch(r'(lsl|lsr|asr|ror) #(\w+)', mod)
        if m:
            n, kind = imm(m.group(2)), m.group(1)
            base = rd(first, W)
            if n == 0:
                return base
            if kind == 'lsl':
                return f'(({t})({base} << {n}))'
            if kind == 'lsr':
                return f'(({t})({base} >> {n}))'
            if kind == 'asr':
                return f'(({t})(({st(W)}){base} >> {n}))'
            return f'(({t})(({base} >> {n}) | ({base} << {W - n})))'
        m = re.fullmatch(r'([us])xt([bhwx])(?: #(\w+))?', mod)
        if m:
            bits = {'b': 8, 'h': 16, 'w': 32, 'x': 64}[m.group(2)]
            src = xreg(first)
            e = f'(uint64_t)(uint{bits}_t){src}' if m.group(1) == 'u' else f'(uint64_t)(int64_t)(int{bits}_t){src}'
            sh = imm(m.group(3)) if m.group(3) else 0
            return f'(({t})({e} << {sh}))'
        raise Unsupported(f'operand modifier {mod}')

    # ---- calls
    def call(self, target_expr, nxt, start, tail=False):
        """A call to guest code at target_expr (an address in the guest's space). It returns to
        its own return address, which the translator stops at (stops: the runtime registers them),
        so the guest stack is as arm64's (an exception unwinds through this function's frame). A
        tail call, or one returning where other code branches to, returns to the translator's
        common stop address (lift_guest_call_stop); a tail call returns to this function's caller
        after it."""
        self.guest_calls[start] = self.guest_calls.get(start, 0) + 1
        if tail:
            return (f'{{ uint64_t lr_ = x30, t_ = {target_expr}; STORE_ALL; lift_guest_call_stop(ts, t_); LOAD_ALL; '
                    f'x30 = lr_; STORE_ALL; return; }}')
        own = not self.foreign(nxt) and nxt - 4 not in self._tainted
        if own:
            self.stops[start].append(nxt)
        return (f'{{ uint64_t t_ = {target_expr}; x30 = BASE + {nxt:#x}ull; STORE_ALL; '
                f'lift_guest_call{"" if own else "_stop"}(ts, t_); LOAD_ALL; x30 = BASE + {nxt:#x}ull; }}')

    def extern_call(self, kind, at):
        if kind == 'sincosf':
            return 'sincosf(v0.s[0], (float *)x0, (float *)x1);'
        if kind in LIBM1:
            return f'{{ float r = {kind}(v0.s[0]); v0 = VZERO; v0.s[0] = r; }}'
        if kind in LIBM2:
            return f'{{ float r = {kind}(v0.s[0], v1.s[0]); v0 = VZERO; v0.s[0] = r; }}'
        if kind in ('memcpy', 'memmove'):
            return 'MEMCOPY(x0, x1, x2);'
        if kind == 'memset':
            return 'MEMSET(x0, (uint8_t)x1, x2);'
        if kind == 'assert':
            return 'lift_fatal_assert(x0, x1, x2);'
        if kind in ('abort', 'stack_chk_fail'):
            return f'lift_fatal("{kind}", {at:#x});'
        raise Unsupported(f'extern kind {kind}')

    def branch(self, t, start, nxt):
        """A goto t, or the tail call a branch to t is."""
        if self.tail_target(t):
            return self.call(f'BASE + {t:#x}ull', nxt, start, tail=True)
        return f'goto L_{t:x};'

    # ---- one instruction
    def insn(self, i, word, start=0):
        mn, ops = i.mnemonic, split_ops(i.op_str)
        nxt = i.address + i.size
        if mn == 'ret':
            if ops and ops[0] != 'x30':
                raise Unsupported('ret to another register')
            return 'STORE_ALL; return;'
        if mn == 'b':
            return self.branch(imm(ops[0]), start, nxt)
        if mn.startswith('b.'):
            return f'if ({CONDS[mn[2:]]}) {self.branch(imm(ops[0]), start, nxt)}'
        if mn in ('cbz', 'cbnz'):
            r = rd(ops[0], width(ops[0]))
            return f'if ({r} {"==" if mn == "cbz" else "!="} 0) {self.branch(imm(ops[1]), start, nxt)}'
        if mn in ('tbz', 'tbnz'):
            return (f'if ((({xreg(ops[0])} >> {imm(ops[1])}) & 1) {"==" if mn == "tbz" else "!="} 0) '
                    f'{self.branch(imm(ops[2]), start, nxt)}')
        if mn == 'bl':
            t = imm(ops[0])
            kind = self.externs.get(t)
            if t in self.funcs and kind is None:
                return f'x30 = BASE + {nxt:#x}ull; STORE_ALL; lift_{t:x}(ts); LOAD_ALL;'
            if kind is None or kind == 'guest':
                return self.call(f'BASE + {t:#x}ull', nxt, start)
            return self.extern_call(kind, i.address)
        if mn == 'br':
            if i.address in self.jumps:
                cases = ' '.join(f'case {t:#x}ull: goto L_{t:x};' for t in self.jumps[i.address][1])
                return f'switch ({xreg(ops[0])} - BASE) {{ {cases} default: lift_fatal("jump", {i.address:#x}); }}'
            # An indirect tail call (as a b to an import's PLT entry: x30 is the caller's).
            return (f'{{ void (*f_)(uint8_t *) = lift_find({xreg(ops[0])}); if (f_) {{ STORE_ALL; f_(ts); return; }} '
                    f'else {self.call(xreg(ops[0]), nxt, start, tail=True)} }}')
        if mn == 'blr':
            # A lifted function (a virtual call to one): called here; anything else, the translator's.
            return (f'{{ void (*f_)(uint8_t *) = lift_find({xreg(ops[0])}); if (f_) {{ x30 = BASE + {nxt:#x}ull; '
                    f'STORE_ALL; f_(ts); LOAD_ALL; }} else {self.call(xreg(ops[0]), nxt, start)} }}')
        if mn in ('nop', 'prfm', 'prfum', 'yield', 'hint', 'bti', 'paciasp', 'autiasp', 'pacibsp', 'autibsp'):
            return ';'
        if mn in ('dmb', 'dsb'):
            return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
        if mn in ('brk', 'udf', 'hlt'):
            return f'lift_fatal("{mn}", {i.address:#x});'
        if mn == 'clrex':
            return 'em_a = ~0ull;'
        if mn in ('adrp', 'adr'):
            return wset(ops[0], f'BASE + {imm(ops[1]):#x}ull')
        if mn == 'mrs':
            if ops[1] != 'tpidr_el0':
                raise Unsupported('system register')
            return wset(ops[0], f'LD64((uint64_t)(ts + {TLS_OFFSET:#x}))')
        if mn in ('ldr', 'ldur', 'str', 'stur'):
            return self.ldst1(mn, ops)
        if mn in NARROW:
            return self.ldst_narrow(mn, ops)
        if mn in ('ldp', 'stp', 'ldnp', 'stnp', 'ldpsw'):
            return self.ldst2(mn, ops)
        if mn in ORDERED:
            return self.ordered(mn, ops)
        if mn in EXCLUSIVE:
            return self.exclusive(mn, ops)
        if mn in EXCLUSIVE_PAIRS:
            return self.exclusive_pair(mn, ops)
        if ATOMIC.fullmatch(mn):
            return self.atomic(mn, ops)
        if mn in ('ld1', 'st1', 'ld1r'):
            return self.simd_ldst(mn, ops)
        simd_move = mn in ('umov', 'smov', 'mov') and '[' in i.op_str
        if ops and is_gpr(ops[0]) and not mn.startswith('f') and not simd_move:
            c = self.integer(mn, ops, word)
            if c is not None:
                return c
        return self.simd(i, mn, ops, word)

    # ---- general-purpose register instructions
    def integer(self, mn, ops, word):
        d = ops[0]
        W = width(d) if d != 'sp' else 64
        t = ut(W)
        if mn == 'mov':
            s = ops[1]
            if s.startswith('#'):
                return wset(d, f'{imm(s) & mask(64):#x}ull')
            if is_gpr(s):
                return wset(d, rd(s, W))
            return None
        if mn == 'movk':
            v = imm(ops[1])
            sh = imm(re.fullmatch(r'lsl #(\w+)', ops[2]).group(1)) if len(ops) > 2 else 0
            return wset(d, f'({t})(({rd(d, W)} & ~{konst(0xFFFF << sh, W)}) | {konst(v << sh, W)})')
        if mn in ('movz', 'movn'):
            v = imm(ops[1])
            sh = imm(re.fullmatch(r'lsl #(\w+)', ops[2]).group(1)) if len(ops) > 2 else 0
            v <<= sh
            return wset(d, konst(~v if mn == 'movn' else v, W))
        if mn in ('add', 'sub', 'adds', 'subs'):
            a = rd(ops[1], W)
            b = self.operand2(ops[2:], W)
            sub = mn.startswith('sub')
            if mn in ('add', 'sub'):
                return wset(d, f'({t})({a} {"-" if sub else "+"} {b})')
            return arith(a, b, W, sub, lambda r: wset(d, r))
        if mn in ('cmp', 'cmn'):
            return arith(rd(ops[0], W), self.operand2(ops[1:], W), W, mn == 'cmp')
        if mn in ('neg', 'negs'):
            b = self.operand2(ops[1:], W)
            if mn == 'neg':
                return wset(d, f'({t})(0 - {b})')
            return arith(f'({t})0', b, W, True, lambda r: wset(d, r))
        if mn in ('adc', 'adcs', 'sbc', 'sbcs', 'ngc', 'ngcs'):
            if mn.startswith('ng'):
                a, b = f'({t})0', rd(ops[1], W)
            else:
                a, b = rd(ops[1], W), rd(ops[2], W)
            if mn.startswith(('sbc', 'ngc')):
                b = f'({t})~{b}'
            # a + b + carry, with arm64's flags (AddWithCarry).
            code = f'{{ {t} fa_ = {a}, fb_ = {b}, fr_ = ({t})(fa_ + fb_ + (cf ? 1 : 0)); '
            if mn.endswith('s'):
                code += (f'unsigned __int128 wide_ = (unsigned __int128)fa_ + fb_ + (cf ? 1 : 0); '
                         f'{flags_nz("fr_", W)} cf = (wide_ >> {W}) & 1; '
                         f'vf = ((~(fa_ ^ fb_) & (fa_ ^ fr_)) >> {W - 1}) & 1; ')
            return code + wset(d, 'fr_') + ' }'
        if mn in ('and', 'orr', 'eor', 'bic', 'orn', 'eon', 'ands', 'bics', 'tst'):
            if mn == 'tst':
                a, b, d = rd(ops[0], W), self.operand2(ops[1:], W), None
            else:
                a, b = rd(ops[1], W), self.operand2(ops[2:], W)
            if mn in ('bic', 'orn', 'eon', 'bics'):
                b = f'({t})~{b}'
            op = {'and': '&', 'ands': '&', 'tst': '&', 'bic': '&', 'bics': '&', 'orr': '|', 'orn': '|',
                  'eor': '^', 'eon': '^'}[mn]
            e = f'({t})({a} {op} {b})'
            if mn in ('ands', 'bics', 'tst'):
                code = f'{{ {t} r_ = {e}; {flags_nz("r_", W)} cf = 0; vf = 0;'
                if d:
                    code += ' ' + wset(d, 'r_')
                return code + ' }'
            return wset(d, e)
        if mn == 'mvn':
            return wset(d, f'({t})~{self.operand2(ops[1:], W)}')
        if mn in ('lsl', 'lsr', 'asr', 'ror'):
            a = rd(ops[1], W)
            if ops[2].startswith('#'):
                return wset(d, self.operand2([ops[1], f'{mn} #{imm(ops[2])}'], W))
            s = f'({rd(ops[2], W)} & {W - 1})'
            if mn == 'lsl':
                return wset(d, f'({t})({a} << {s})')
            if mn == 'lsr':
                return wset(d, f'({t})({a} >> {s})')
            if mn == 'asr':
                return wset(d, f'({t})(({st(W)}){a} >> {s})')
            return wset(d, f'({t})({s} ? ({a} >> {s}) | ({a} << ({W} - {s})) : {a})')
        if mn == 'bfc':
            lsb, w = imm(ops[1]), imm(ops[2])
            return wset(d, f'({t})({rd(d, W)} & ~{konst(mask(w) << lsb, W)})')
        if mn in ('ubfx', 'sbfx', 'ubfiz', 'sbfiz', 'bfi', 'bfxil'):
            a, lsb, w = rd(ops[1], W), imm(ops[2]), imm(ops[3])
            field = konst(mask(w), W)
            if mn == 'ubfx':
                return wset(d, f'({t})(({a} >> {lsb}) & {field})')
            if mn == 'sbfx':
                return wset(d, f'({t})((({st(W)})({t})({a} << {W - lsb - w})) >> {W - w})')
            if mn == 'ubfiz':
                return wset(d, f'({t})(({a} & {field}) << {lsb})')
            if mn == 'sbfiz':
                return wset(d, f'({t})(({t})((({st(W)})({t})({a} << {W - w})) >> {W - w}) << {lsb})')
            if mn == 'bfi':
                return wset(d, f'({t})(({rd(d, W)} & ~{konst(mask(w) << lsb, W)}) | (({a} & {field}) << {lsb}))')
            return wset(d, f'({t})(({rd(d, W)} & ~{field}) | (({a} >> {lsb}) & {field}))')
        if mn in ('sxtb', 'sxth', 'sxtw', 'uxtb', 'uxth'):
            bits = {'b': 8, 'h': 16, 'w': 32}[mn[-1]]
            src = xreg(ops[1])
            if mn[0] == 's':
                return wset(d, f'({t})(int64_t)(int{bits}_t){src}')
            return wset(d, f'({t})(uint{bits}_t){src}')
        if mn == 'extr':
            a, b, lsb = rd(ops[1], W), rd(ops[2], W), imm(ops[3])
            if lsb == 0:
                return wset(d, b)
            return wset(d, f'({t})(({b} >> {lsb}) | ({a} << {W - lsb}))')
        if mn in ('csel', 'csinc', 'csinv', 'csneg'):
            a, b, c = rd(ops[1], W), rd(ops[2], W), CONDS[ops[3]]
            alt = {'csel': b, 'csinc': f'({t})({b} + 1)', 'csinv': f'({t})~{b}', 'csneg': f'({t})(0 - {b})'}[mn]
            return wset(d, f'({c}) ? {a} : {alt}')
        if mn in ('cset', 'csetm'):
            return wset(d, f'({CONDS[ops[1]]}) ? {konst(1 if mn == "cset" else -1, W)} : ({t})0')
        if mn in ('cinc', 'cinv', 'cneg'):
            a, c = rd(ops[1], W), CONDS[ops[2]]
            alt = {'cinc': f'({t})({a} + 1)', 'cinv': f'({t})~{a}', 'cneg': f'({t})(0 - {a})'}[mn]
            return wset(d, f'({c}) ? {alt} : {a}')
        if mn in ('ccmp', 'ccmn'):
            a = rd(ops[0], W)
            b = konst(imm(ops[1]), W) if ops[1].startswith('#') else rd(ops[1], W)
            nzcv, c = imm(ops[2]), CONDS[ops[3]]
            return (f'if ({c}) {arith(a, b, W, mn == "ccmp")} else {{ n = {nzcv >> 3 & 1}; z = {nzcv >> 2 & 1}; '
                    f'cf = {nzcv >> 1 & 1}; vf = {nzcv & 1}; }}')
        if mn in ('mul', 'madd', 'msub', 'mneg'):
            a, b = rd(ops[1], W), rd(ops[2], W)
            if mn == 'mul':
                return wset(d, f'({t})({a} * {b})')
            if mn == 'mneg':
                return wset(d, f'({t})(0 - {a} * {b})')
            c = rd(ops[3], W)
            return wset(d, f'({t})({c} {"+" if mn == "madd" else "-"} {a} * {b})')
        if mn in ('smaddl', 'umaddl', 'smsubl', 'umsubl', 'smull', 'umull', 'smnegl', 'umnegl'):
            cast = '(uint64_t)(int64_t)(int32_t)' if mn[0] == 's' else '(uint64_t)(uint32_t)'
            p = f'({cast}{xreg(ops[1])} * {cast}{xreg(ops[2])})'
            if mn.endswith('ull'):
                return wset(d, p)
            if mn.endswith('negl'):
                return wset(d, f'(0 - {p})')
            return wset(d, f'{xreg(ops[3])} {"+" if "add" in mn else "-"} {p}')
        if mn == 'smulh':
            return wset(d, f'(uint64_t)(((__int128)(int64_t){xreg(ops[1])} * (__int128)(int64_t){xreg(ops[2])}) >> 64)')
        if mn == 'umulh':
            return wset(d, f'(uint64_t)(((unsigned __int128){xreg(ops[1])} * (unsigned __int128){xreg(ops[2])}) >> 64)')
        if mn in ('udiv', 'sdiv'):
            a, b = rd(ops[1], W), rd(ops[2], W)
            if mn == 'udiv':
                return wset(d, f'{b} ? ({t})({a} / {b}) : ({t})0')
            s = st(W)
            return wset(d, f'{b} == 0 ? ({t})0 : (({s}){b} == -1) ? ({t})(0 - {a}) : ({t})(({s}){a} / ({s}){b})')
        if mn in ('clz', 'cls', 'rbit', 'rev', 'rev16', 'rev32'):
            a = rd(ops[1], W)
            sfx = '' if W == 32 else 'll'
            if mn == 'clz':
                return wset(d, f'{a} ? ({t})__builtin_clz{sfx}({a}) : ({t}){W}')
            if mn == 'cls':  # leading bits after the sign equal to it: the zeros of the pairs' xor
                return wset(d, f'({t})__builtin_clz{sfx}(({t})({a} ^ ({t})({a} << 1)) | 1)')
            if mn == 'rbit':
                return wset(d, f'({t})__builtin_bitreverse{W}({a})')
            if mn == 'rev':
                return wset(d, f'({t})__builtin_bswap{W}({a})')
            if mn == 'rev16':
                m = konst(0x00FF00FF00FF00FF, W)
                return wset(d, f'({t})((({a} >> 8) & {m}) | (({a} & {m}) << 8))')
            return wset(d, f'(uint64_t)__builtin_bswap32((uint32_t){a}) | ((uint64_t)__builtin_bswap32((uint32_t)({a} >> 32)) << 32)')
        raise Unsupported('instruction')

    def fcvt_int(self, mn, ops):
        """fcvt{z,p,m,n,a}{s,u} to a general register: rounded then saturated, NaN 0."""
        d, s = ops[0], fpop(ops[1])
        if len(ops) > 2:
            raise Unsupported('fixed-point conversion')
        rnd = {'z': 'trunc', 'p': 'ceil', 'm': 'floor', 'n': 'nearbyint', 'a': 'round'}[mn[4]]
        W = width(d)
        return wset(d, f'CVT_{"I" if mn[5] == "s" else "U"}{W}({rnd}((double){lane(s, 0)}))')

    def ldst1(self, mn, ops):
        r = ops[0]
        addr, wb = self.mem(ops, 1)
        load = mn in ('ldr', 'ldur')
        m = re.fullmatch(r'([xwsdqbh])(\d+)|wzr|xzr', r)
        if not m:
            raise Unsupported('register')
        kind = 'w' if r == 'wzr' else 'x' if r == 'xzr' else m.group(1)
        if kind in 'xw':
            if load:
                return f'{wset(r, f"LD64({addr})" if kind == "x" else f"LD32({addr})")} {wb}'
            val = xreg(r)
            return f'{"ST64" if kind == "x" else "ST32"}({addr}, {val}); {wb}'
        n = int(m.group(2))
        field, bits = {'b': ('b', 8), 'h': ('h', 16), 's': ('w', 32), 'd': ('x', 64)}.get(kind, (None, 128))
        if load:
            if field:
                return f'{{ uint64_t a_ = {addr}; v{n} = VZERO; v{n}.{field}[0] = LD{bits}(a_); }} {wb}'
            return f'LDQ(&v{n}, {addr}); {wb}'
        if field:
            return f'ST{bits}({addr}, v{n}.{field}[0]); {wb}'
        return f'STQ({addr}, &v{n}); {wb}'

    def ldst_narrow(self, mn, ops):
        """Byte, halfword and sign-extending loads, byte and halfword stores."""
        load, size, signed = NARROW[mn]
        r = ops[0]
        addr, wb = self.mem(ops, 1)
        if not re.fullmatch(r'[xw](\d+)|wzr|xzr', r):
            raise Unsupported('register')
        bits = size * 8
        if not load:
            return f'ST{bits}({addr}, (uint{bits}_t){xreg(r)}); {wb}'
        value = f'LD{bits}({addr})'
        if signed:
            value = f'(uint64_t)(int64_t)(int{bits}_t){value}'
        return f'{wset(r, value)} {wb}'

    def ordered(self, mn, ops):
        load, size = ORDERED[mn]
        r = ops[0]
        bits = size * 8 or width(r)
        addr, wb = self.mem(ops, 1)
        if wb:
            raise Unsupported('writeback')
        if load:
            return wset(r, f'LDA{bits}({addr})')
        return f'STL{bits}({addr}, ({ut(bits)}){xreg(r)});'

    def exclusive(self, mn, ops):
        load, size = EXCLUSIVE[mn]
        if load:
            r = ops[0]
            bits = size * 8 or width(r)
            addr, _ = self.mem(ops, 1)
            return f'{{ uint64_t a_ = {addr}; em_a = a_; em_v = LDA{bits}(a_); {wset(r, "em_v")} }}'
        status, r = ops[0], ops[1]
        bits = size * 8 or width(r)
        addr, _ = self.mem(ops, 2)
        ok = f'em_a == a_ && CAS{bits}(a_, ({ut(bits)})em_v, ({ut(bits)}){xreg(r)})'
        return f'{{ uint64_t a_ = {addr}; {wset(status, f"({ok}) ? 0 : 1")} em_a = ~0ull; }}'

    def exclusive_pair(self, mn, ops):
        if mn in ('ldxp', 'ldaxp'):
            r1, r2 = ops[0], ops[1]
            if is_w(r1):
                raise Unsupported('32-bit pair')
            addr, _ = self.mem(ops, 2)
            return (f'{{ uint64_t a_ = {addr}; em_a = a_; em_q = LDA128(a_); {wset(r1, "(uint64_t)em_q")} '
                    f'{wset(r2, "(uint64_t)(em_q >> 64)")} }}')
        status, r1, r2 = ops[0], ops[1], ops[2]
        if is_w(r1):
            raise Unsupported('32-bit pair')
        addr, _ = self.mem(ops, 3)
        new = f'((unsigned __int128){xreg(r2)} << 64 | {xreg(r1)})'
        return (f'{{ uint64_t a_ = {addr}; {wset(status, f"(em_a == a_ && CAS128(a_, em_q, {new})) ? 0 : 1")} '
                f'em_a = ~0ull; }}')

    def atomic(self, mn, ops):
        """LSE atomics: ld<op>, st<op>, swp, cas (any ordering: all sequentially consistent here)."""
        m = ATOMIC.fullmatch(mn)
        kind, size = m.group(2) or m.group(3) or m.group(4), m.group(6)
        if m.group(1) == 'st':  # st<op>: ld<op> into zr
            ops = [ops[0], 'xzr', ops[1]]
        if kind == 'cas':
            s, n = ops[0], ops[1]
            bits = {'b': 8, 'h': 16}.get(size) or width(s)
            addr, _ = self.mem(ops, 2)
            return (f'{{ {ut(bits)} e_ = ({ut(bits)}){xreg(s)}; CAS{bits}V({addr}, &e_, ({ut(bits)}){xreg(n)}); '
                    f'{wset(s, "e_")} }}')
        s, r = ops[0], ops[1]
        bits = {'b': 8, 'h': 16}.get(size) or width(s)
        addr, _ = self.mem(ops, 2)
        return (f'{{ uint64_t a_ = {addr}; {ut(bits)} v_ = ({ut(bits)}){xreg(s)}, o_ = LDA{bits}(a_); '
                f'while (!CAS{bits}V(a_, &o_, ({ut(bits)})({LSE[kind]}))) {{}} {wset(r, "o_")} }}')

    def ldst2(self, mn, ops):
        r1, r2 = ops[0], ops[1]
        addr, wb = self.mem(ops, 2)
        load = mn in ('ldp', 'ldnp', 'ldpsw')
        k1 = re.fullmatch(r'([xwsdq])(\d+)|wzr|xzr', r1)
        kind = k1.group(1) if k1.group(1) else ('w' if r1 == 'wzr' else 'x')
        size = {'x': 8, 'w': 4, 's': 4, 'd': 8, 'q': 16}[kind]
        out = [f'{{ uint64_t a_ = {addr};']
        for j, r in enumerate((r1, r2)):
            a = f'a_ + {j * size}'
            if kind in 'xw':
                if load:
                    v = f'LD64({a})' if kind == 'x' else f'LD32({a})'
                    if mn == 'ldpsw':
                        v = f'(uint64_t)(int64_t)(int32_t)LD32({a})'
                    out.append(wset(r, v))
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

    def simd_ldst(self, mn, ops):
        """ld1/st1 of one lane, of whole registers, ld1r (load and replicate)."""
        regs, rest = ops[0], ops[1:]
        m = re.fullmatch(r'\{(.*)\}(?:\[(\d+)\])?', regs)
        if not m:
            raise Unsupported('register list')
        names = [s.strip() for s in m.group(1).split(',')]
        addr, wb = self.mem(rest, 0)
        if m.group(2) is not None:  # one lane
            if len(names) != 1:
                raise Unsupported('lanes of several registers')
            r = re.fullmatch(r'v(\d+)\.([bhsd])', names[0])
            n, es, k = int(r.group(1)), r.group(2), int(m.group(2))
            f, bits = UFIELD[es], BITS[es]
            if mn == 'ld1':
                return f'v{n}.{f}[{k}] = LD{bits}({addr}); {wb}'
            return f'ST{bits}({addr}, v{n}.{f}[{k}]); {wb}'
        vs = [fpop(s) for s in names]
        if mn == 'ld1r':
            if len(vs) != 1:
                raise Unsupported('ld1r of several registers')
            v = vs[0]
            bits = BITS[v[3]]
            return f'{{ {ut(bits)} r_ = LD{bits}({addr}); {put(v, ["r_"] * v[2], "u")} }} {wb}'
        out = [f'{{ uint64_t a_ = {addr};']
        at = 0
        for v in vs:
            total = v[2] * BITS[v[3]] // 8
            if total == 16:
                out.append(f'LDQ(&v{v[1]}, a_ + {at});' if mn == 'ld1' else f'STQ(a_ + {at}, &v{v[1]});')
            elif mn == 'ld1':
                out.append(f'v{v[1]} = VZERO; v{v[1]}.x[0] = LD64(a_ + {at});')
            else:
                out.append(f'ST64(a_ + {at}, v{v[1]}.x[0]);')
            at += total
        out.append('}')
        return ' '.join(out) + (' ' + wb if wb else '')

    # ---- floating point and SIMD
    def fbin(self, ops, f):
        """A lane-wise float operation of two operands (a vector, a scalar, or by element)."""
        d, a, b = (fpop(o) for o in ops)
        es = esize(d)
        return put(d, [f(lane(a, k), lane(b, k), es) for k in range(nlanes(d))])

    def simd(self, i, mn, ops, word):
        F = lambda es, name: name + ('f' if es == 's' else '')  # libm's name by precision
        if mn == 'fadd':
            return self.fbin(ops, lambda a, b, es: f'{a} + {b}')
        if mn == 'fsub':
            return self.fbin(ops, lambda a, b, es: f'{a} - {b}')
        if mn == 'fmul':
            return self.fbin(ops, lambda a, b, es: f'{a} * {b}')
        if mn == 'fdiv':
            return self.fbin(ops, lambda a, b, es: f'{a} / {b}')
        if mn == 'fnmul':
            return self.fbin(ops, lambda a, b, es: f'-({a} * {b})')
        if mn == 'fabd':
            return self.fbin(ops, lambda a, b, es: f'{F(es, "fabs")}({a} - {b})')
        if mn in ('fmax', 'fmin', 'fmaxnm', 'fminnm'):
            return self.fbin(ops, lambda a, b, es: f'A64_{mn.upper()}{es.upper()}({a}, {b})')
        if mn in ('fmla', 'fmls'):
            d, a, b = (fpop(o) for o in ops)
            es = esize(d)
            sign = '' if mn == 'fmla' else '-'
            return put(d, [f'{F(es, "fma")}({sign}{lane(a, k)}, {lane(b, k)}, {lane(d, k)})' for k in range(nlanes(d))])
        if mn in ('fmadd', 'fmsub', 'fnmadd', 'fnmsub'):
            d, a, b, c = (fpop(o) for o in ops)
            fma = F(esize(d), 'fma')
            ra, rb, rc = lane(a, 0), lane(b, 0), lane(c, 0)
            e = {'fmadd': f'{fma}({ra}, {rb}, {rc})', 'fmsub': f'{fma}(-{ra}, {rb}, {rc})',
                 'fnmadd': f'{fma}(-{ra}, {rb}, -{rc})', 'fnmsub': f'{fma}({ra}, {rb}, -{rc})'}[mn]
            return put(d, [e])
        if mn in ('fneg', 'fabs'):  # the sign bit, NaNs too
            d, a = fpop(ops[0]), fpop(ops[1])
            b = BITS[esize(d)]
            sign = f'(({ut(b)})1 << {b - 1})'
            op = (lambda x: f'{x} ^ {sign}') if mn == 'fneg' else (lambda x: f'{x} & ~{sign}')
            return put(d, [op(lane(a, k, 'u')) for k in range(nlanes(d))], 'u')
        if mn == 'fsqrt' or mn.startswith('frint'):
            d, a = fpop(ops[0]), fpop(ops[1])
            es = esize(d)
            name = {'fsqrt': 'sqrt', 'frintm': 'floor', 'frintp': 'ceil', 'frintz': 'trunc', 'frinta': 'round',
                    'frintn': 'nearbyint', 'frinti': 'nearbyint', 'frintx': 'rint'}.get(mn)
            if not name:
                raise Unsupported('rounding')
            return put(d, [f'{F(es, name)}({lane(a, k)})' for k in range(nlanes(d))])
        if mn == 'faddp':
            if len(ops) == 2:  # scalar: the pair of a 2-lane vector
                d, a = fpop(ops[0]), fpop(ops[1])
                if a[0] != 'vec' or a[2] != 2:
                    raise Unsupported('faddp form')
                return put(d, [f'{lane(a, 0)} + {lane(a, 1)}'])
            d, a, b = (fpop(o) for o in ops)
            L = nlanes(d)
            src = [lane(a, k) for k in range(L)] + [lane(b, k) for k in range(L)]
            return put(d, [f'{src[2 * k]} + {src[2 * k + 1]}' for k in range(L)])
        if mn == 'fmov':
            return self.fmov(ops, word)
        if mn in ('fcmp', 'fcmpe'):
            a = lane(fpop(ops[0]), 0)
            b = '0.0' if ops[1].startswith('#') else lane(fpop(ops[1]), 0)
            return f'FLAGS_FCMP({a}, {b});'
        if mn in ('fccmp', 'fccmpe'):
            a, b = lane(fpop(ops[0]), 0), lane(fpop(ops[1]), 0)
            nzcv = imm(ops[2])
            return (f'if ({CONDS[ops[3]]}) {{ FLAGS_FCMP({a}, {b}); }} else {{ n = {nzcv >> 3 & 1}; z = {nzcv >> 2 & 1}; '
                    f'cf = {nzcv >> 1 & 1}; vf = {nzcv & 1}; }}')
        if mn == 'fcsel':
            d, a, b = (fpop(o) for o in ops[:3])
            return put(d, [f'({CONDS[ops[3]]}) ? {lane(a, 0, "u")} : {lane(b, 0, "u")}'], 'u')
        if mn in ('fcmeq', 'fcmge', 'fcmgt', 'fcmle', 'fcmlt', 'facge', 'facgt'):
            d, a = fpop(ops[0]), fpop(ops[1])
            zero = ops[2].startswith('#')
            b = None if zero else fpop(ops[2])
            es = esize(d)
            rel = {'fcmeq': '==', 'fcmge': '>=', 'fcmgt': '>', 'fcmle': '<=', 'fcmlt': '<', 'facge': '>=', 'facgt': '>'}[mn]
            ones = konst(-1, BITS[es])

            def cmp(k):
                x, y = lane(a, k), ('0.0' if zero else lane(b, k))
                if mn.startswith('fac'):
                    x, y = f'{F(es, "fabs")}({x})', f'{F(es, "fabs")}({y})'
                return f'({x} {rel} {y}) ? {ones} : 0'
            return put(d, [cmp(k) for k in range(nlanes(d))], 'u')
        if mn in ('fcvtl', 'fcvtl2', 'fcvtn', 'fcvtn2'):
            d, s = fpop(ops[0]), fpop(ops[1])
            L = nlanes(d)
            if mn.startswith('fcvtl'):  # .2d from the lower (upper, 2) two lanes of a .4s/.2s
                if esize(d) != 'd':
                    raise Unsupported('half-precision')
                first = 2 if mn == 'fcvtl2' else 0
                return put(d, [f'(double){lane(s, first + k)}' for k in range(L)])
            if esize(d) != 's':
                raise Unsupported('half-precision')
            if mn == 'fcvtn':  # .2s from a .2d, upper half cleared
                return put(d, [f'(float){lane(s, k)}' for k in range(L)])
            return ('{ ' + ' '.join(f'float t{k} = (float){lane(s, k)};' for k in range(2)) +
                    f' v{d[1]}.s[2] = t0; v{d[1]}.s[3] = t1; }}')  # fcvtn2: into the upper half
        if mn == 'fcvt':
            d, s = fpop(ops[0]), fpop(ops[1])
            if d[0] == 's' and s[0] == 'd':
                return put(d, [f'(float){lane(s, 0)}'])
            if d[0] == 'd' and s[0] == 's':
                return put(d, [f'(double){lane(s, 0)}'])
            raise Unsupported('half-precision')
        if re.fullmatch(r'fcvt[zpmna][su]', mn):
            if is_gpr(ops[0]):
                return self.fcvt_int(mn, ops)
            d, s = fpop(ops[0]), fpop(ops[1])
            if len(ops) > 2:
                raise Unsupported('fixed-point conversion')
            rnd = {'z': 'trunc', 'p': 'ceil', 'm': 'floor', 'n': 'nearbyint', 'a': 'round'}[mn[4]]
            es = esize(d)
            conv = f'CVT_{"I" if mn[5] == "s" else "U"}{BITS[es]}'
            return put(d, [f'({ut(BITS[es])}){conv}({rnd}((double){lane(s, k)}))' for k in range(nlanes(d))], 'u')
        if mn in ('scvtf', 'ucvtf'):
            d = fpop(ops[0])
            if len(ops) > 2:
                raise Unsupported('fixed-point conversion')
            ctype = 'float' if esize(d) == 's' else 'double'
            if is_gpr(ops[1]):
                W = width(ops[1])
                src = f'({st(W) if mn == "scvtf" else ut(W)}){rd(ops[1], W)}'
                return put(d, [f'({ctype}){src}'])
            s = fpop(ops[1])
            kind = 'i' if mn == 'scvtf' else 'u'
            return put(d, [f'({ctype}){lane(s, k, kind)}' for k in range(nlanes(d))])
        if mn in ('movi', 'mvni'):
            d = ops[0]
            v = adv_simd_imm(word)
            if mn == 'mvni':
                v = ~v & mask(64)
            if re.fullmatch(r'd\d+', d):
                return f'v{d[1:]}.x[0] = {v:#x}ull; v{d[1:]}.x[1] = 0;'
            dv = fpop(d)
            hi = v if dv[2] * BITS[dv[3]] == 128 else 0
            return f'v{dv[1]}.x[0] = {v:#x}ull; v{dv[1]}.x[1] = {hi:#x}ull;'
        if mn in ('orr', 'bic') and len(ops) >= 2 and ops[1].startswith('#'):  # vector, immediate
            dv = fpop(ops[0])
            v = adv_simd_imm(word)
            n = dv[1]
            op = '|' if mn == 'orr' else '& ~'
            sets = f'v{n}.x[0] = v{n}.x[0] {op}{v:#x}ull;'
            if dv[2] * BITS[dv[3]] == 128:
                return sets + f' v{n}.x[1] = v{n}.x[1] {op}{v:#x}ull;'
            return sets + f' v{n}.x[1] = 0;'
        if mn in ('and', 'orr', 'eor', 'bic', 'orn', 'bsl', 'bit', 'bif', 'mvn', 'not', 'mov') and \
                all(re.fullmatch(r'v\d+\.(8|16)b', o) for o in ops):
            return self.bitwise(mn, [fpop(o) for o in ops])
        if mn == 'dup':
            d = fpop(ops[0])
            if is_gpr(ops[1]):
                return put(d, [f'({ut(BITS[esize(d)])}){xreg(ops[1])}'] * nlanes(d), 'u')
            s = fpop(ops[1])
            if s[0] != 'el':
                raise Unsupported('dup form')
            return put(d, [lane(s, 0, 'u')] * nlanes(d), 'u')
        if mn in ('mov', 'ins', 'umov', 'smov'):
            return self.simd_mov(mn, ops)
        if mn in ('zip1', 'zip2', 'trn1', 'trn2', 'uzp1', 'uzp2', 'rev64', 'rev32', 'rev16', 'ext'):
            return self.permute(mn, ops)
        if mn in ('add', 'sub', 'mul', 'cmeq', 'cmhi', 'cmhs', 'cmgt', 'cmge', 'cmle', 'cmlt', 'cmtst', 'umax',
                  'umin', 'smax', 'smin', 'neg', 'abs', 'shl', 'ushr', 'sshr', 'usra', 'ssra'):
            return self.vint(mn, ops)
        if mn in ('cnt', 'uaddlv', 'saddlv', 'addv', 'umaxv', 'uminv', 'smaxv', 'sminv', 'addp'):
            return self.reduce(mn, ops)
        if mn in ('ushll', 'sshll', 'ushll2', 'sshll2', 'uxtl', 'sxtl', 'uxtl2', 'sxtl2', 'xtn', 'xtn2'):
            return self.widen(mn, ops)
        raise Unsupported('instruction')

    def fmov(self, ops, word):
        d, s = ops
        if is_gpr(d):  # to a general register: the bits
            return wset(d, lane(fpop(s), 0, 'u'))
        dv = fpop(d)
        if is_gpr(s):
            if dv[0] == 'el':  # fmov v0.d[1], x1: the upper half only
                return f'v{dv[1]}.x[{dv[3]}] = {xreg(s)};'
            return put(dv, [f'({ut(BITS[esize(dv)])}){xreg(s)}'], 'u')
        if s.startswith('#'):
            bits = fp_imm8(word, esize(dv) == 'd', dv[0] == 'vec')
            return put(dv, [f'{bits:#x}ull'] * nlanes(dv), 'u')
        return put(dv, [lane(fpop(s), 0, 'u')], 'u')

    def simd_mov(self, mn, ops):
        d, s = ops
        if is_gpr(d):  # umov/smov/mov to a general register
            src = fpop(s)
            if mn == 'smov':
                return wset(d, f'(uint64_t)(int64_t){lane(src, 0, "i")}')
            return wset(d, f'(uint64_t){lane(src, 0, "u")}')
        dv = fpop(d)
        if is_gpr(s):  # ins from a general register
            if dv[0] != 'el':
                raise Unsupported('mov form')
            return f'v{dv[1]}.{UFIELD[dv[2]]}[{dv[3]}] = ({ut(BITS[dv[2]])}){xreg(s)};'
        sv = fpop(s)
        if dv[0] == 'el' and sv[0] == 'el':
            if dv[2] != sv[2]:
                raise Unsupported('element sizes')
            f = UFIELD[dv[2]]
            return f'v{dv[1]}.{f}[{dv[3]}] = v{sv[1]}.{f}[{sv[3]}];'
        if dv[0] in 'bhsd' and sv[0] == 'el':  # mov s0, v1.s[2]: a scalar from an element
            return put(dv, [lane(sv, 0, 'u')], 'u')
        raise Unsupported('mov form')

    def bitwise(self, mn, vs):
        d = vs[0]
        halves = 2 if d[2] == 16 else 1
        exprs = []
        for k in range(halves):
            def r(v):
                return f'v{v[1]}.x[{k}]'
            if mn in ('mvn', 'not'):
                e = f'~{r(vs[1])}'
            elif mn == 'mov':
                e = r(vs[1])
            else:
                a, b = r(vs[1]), r(vs[2])
                e = {'and': f'{a} & {b}', 'orr': f'{a} | {b}', 'eor': f'{a} ^ {b}', 'bic': f'{a} & ~{b}',
                     'orn': f'{a} | ~{b}', 'bsl': f'({r(d)} & {a}) | (~{r(d)} & {b})',
                     'bit': f'({r(d)} & ~{b}) | ({a} & {b})', 'bif': f'({r(d)} & {b}) | ({a} & ~{b})'}[mn]
            exprs.append(e)
        tmp = ' '.join(f'uint64_t t{k} = {e};' for k, e in enumerate(exprs))
        n = d[1]
        if halves == 2:
            return f'{{ {tmp} v{n}.x[0] = t0; v{n}.x[1] = t1; }}'
        return f'{{ {tmp} v{n}.x[0] = t0; v{n}.x[1] = 0; }}'

    def vint(self, mn, ops):
        """Lane-wise integer vector operations (and their scalar d forms)."""
        d = fpop(ops[0])
        W = BITS[esize(d)]
        t = ut(W)
        L = nlanes(d)
        a = fpop(ops[1])
        if mn in ('neg', 'abs'):
            if mn == 'neg':
                return put(d, [f'({t})(0 - {lane(a, k, "u")})' for k in range(L)], 'u')
            return put(d, [f'({t})({lane(a, k, "i")} < 0 ? 0 - {lane(a, k, "u")} : {lane(a, k, "u")})'
                           for k in range(L)], 'u')
        if mn in ('shl', 'ushr', 'sshr', 'usra', 'ssra'):
            n = imm(ops[2])
            if mn == 'shl':
                f = lambda k: f'({t})({lane(a, k, "u")} << {n})'
            elif mn in ('ushr', 'usra'):
                f = lambda k: f'({t})({lane(a, k, "u")} >> {n})' if n < W else '0'
            else:
                f = lambda k: f'({t})({lane(a, k, "i")} >> {min(n, W - 1)})'
            if mn.endswith('sra'):
                g = f
                f = lambda k: f'({t})({lane(d, k, "u")} + {g(k)})'
            return put(d, [f(k) for k in range(L)], 'u')
        ones = konst(-1, W)
        if len(ops) > 2 and ops[2].startswith('#'):  # compare with zero
            rel = {'cmeq': '==', 'cmge': '>=', 'cmgt': '>', 'cmle': '<=', 'cmlt': '<'}[mn]
            return put(d, [f'({lane(a, k, "i")} {rel} 0) ? {ones} : 0' for k in range(L)], 'u')
        b = fpop(ops[2])
        if mn in ('add', 'sub', 'mul'):
            op = {'add': '+', 'sub': '-', 'mul': '*'}[mn]
            return put(d, [f'({t})({lane(a, k, "u")} {op} {lane(b, k, "u")})' for k in range(L)], 'u')
        if mn in ('umax', 'umin', 'smax', 'smin'):
            kind = 'u' if mn[0] == 'u' else 'i'
            rel = '>' if mn.endswith('max') else '<'
            return put(d, [f'({t})({lane(a, k, kind)} {rel} {lane(b, k, kind)} ? {lane(a, k, kind)} : {lane(b, k, kind)})'
                           for k in range(L)], 'u')
        if mn == 'cmtst':
            return put(d, [f'({lane(a, k, "u")} & {lane(b, k, "u")}) ? {ones} : 0' for k in range(L)], 'u')
        kind = 'u' if mn in ('cmhi', 'cmhs', 'cmeq') else 'i'
        rel = {'cmeq': '==', 'cmhi': '>', 'cmhs': '>=', 'cmgt': '>', 'cmge': '>='}[mn]
        return put(d, [f'({lane(a, k, kind)} {rel} {lane(b, k, kind)}) ? {ones} : 0' for k in range(L)], 'u')

    def reduce(self, mn, ops):
        if mn == 'cnt':
            d, a = fpop(ops[0]), fpop(ops[1])
            return put(d, [f'(uint8_t)__builtin_popcount({lane(a, k, "u")})' for k in range(nlanes(d))], 'u')
        if mn == 'addp' and len(ops) == 3:
            d, a, b = (fpop(o) for o in ops)
            L = nlanes(d)
            t = ut(BITS[esize(d)])
            src = [lane(a, k, 'u') for k in range(L)] + [lane(b, k, 'u') for k in range(L)]
            return put(d, [f'({t})({src[2 * k]} + {src[2 * k + 1]})' for k in range(L)], 'u')
        d, a = fpop(ops[0]), fpop(ops[1])
        t = ut(BITS[esize(d)])
        if mn == 'addp':  # scalar: addp d0, v1.2d
            return put(d, [f'({t})({lane(a, 0, "u")} + {lane(a, 1, "u")})'], 'u')
        kind = 'i' if mn[0] == 's' else 'u'
        xs = [lane(a, k, kind) for k in range(nlanes(a))]
        if mn in ('uaddlv', 'saddlv', 'addv'):
            wide = st(BITS[esize(d)]) if kind == 'i' else t
            return put(d, [f'({t})({" + ".join(f"({wide}){x}" for x in xs)})'], 'u')
        pick = '>' if mn.endswith('maxv') else '<'
        e = xs[0]
        for x in xs[1:]:
            e = f'({x} {pick} {e} ? {x} : {e})'
        return put(d, [f'({t}){e}'], 'u')

    def widen(self, mn, ops):
        d, a = fpop(ops[0]), fpop(ops[1])
        L = nlanes(d)
        if mn.startswith('xtn'):
            t = ut(BITS[esize(d)])
            if mn == 'xtn2':  # into the upper half of d
                f, h = UFIELD[esize(d)], L // 2
                return ('{ ' + ' '.join(f'{t} t{k} = ({t}){lane(a, k, "u")};' for k in range(h)) + ' ' +
                        ' '.join(f'v{d[1]}.{f}[{h + k}] = t{k};' for k in range(h)) + ' }')
            return put(d, [f'({t}){lane(a, k, "u")}' for k in range(L)], 'u')
        shift = imm(ops[2]) if len(ops) > 2 else 0
        kind = 'i' if mn[0] == 's' else 'u'
        t = ut(BITS[esize(d)])
        first = L if mn.endswith('2') else 0  # the source lanes: a's upper half for ...2
        return put(d, [f'({t})(({t}){lane(a, first + k, kind)} << {shift})' for k in range(L)], 'u')

    def permute(self, mn, ops):
        d = fpop(ops[0])
        n = d[1]
        if mn == 'ext':
            a, b = fpop(ops[1]), fpop(ops[2])
            k = imm(ops[3])
            total = d[2]  # bytes: 8 or 16
            if d[3] != 'b':
                raise Unsupported('ext form')
            return (f'{{ uint8_t t[32]; memcpy(t, v{a[1]}.b, {total}); memcpy(t + {total}, v{b[1]}.b, {total}); '
                    f'{"v%d = VZERO; " % n if total == 8 else ""}memcpy(v{n}.b, t + {k}, {total}); }}')
        L, es = d[2], d[3]
        f = UFIELD[es]
        if mn in ('rev64', 'rev32', 'rev16'):
            a = fpop(ops[1])
            group = {'rev64': 64, 'rev32': 32, 'rev16': 16}[mn] // BITS[es]
            src = [f'v{a[1]}.{f}[{k ^ (group - 1)}]' for k in range(L)]
        else:
            a, b = fpop(ops[1]), fpop(ops[2])
            A = [f'v{a[1]}.{f}[{k}]' for k in range(L)]
            B = [f'v{b[1]}.{f}[{k}]' for k in range(L)]
            h = L // 2
            if mn == 'zip1':
                src = [x for k in range(h) for x in (A[k], B[k])]
            elif mn == 'zip2':
                src = [x for k in range(h, L) for x in (A[k], B[k])]
            elif mn == 'trn1':
                src = [x for k in range(0, L, 2) for x in (A[k], B[k])]
            elif mn == 'trn2':
                src = [x for k in range(1, L, 2) for x in (A[k], B[k])]
            elif mn == 'uzp1':
                src = (A + B)[0::2]
            else:
                src = (A + B)[1::2]
        ctype = CTYPE[f]
        tmp = ' '.join(f'{ctype} t{k} = {e};' for k, e in enumerate(src))
        clear = f'v{n} = VZERO; ' if L * BITS[es] < 128 else ''
        sets = ' '.join(f'v{n}.{f}[{k}] = t{k};' for k in range(L))
        return f'{{ {tmp} {clear}{sets} }}'


PRELUDE = r'''#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wunused-function"  // the helpers a library's code doesn't use

// Built for FMA, function by function (arm64's fused multiply-adds are fmaf, an instruction only
// with it): nothing here may run before the CPU is known to have it.
#pragma clang attribute push(__attribute__((target("fma,cx16"))), apply_to = function)

typedef union { float s[4]; double d[2]; uint8_t b[16]; uint16_t h[8]; uint32_t w[4]; uint64_t x[2]; } vreg;
static const vreg VZERO;
#ifndef COVER
#define COVER(a)
#endif
#ifndef LIFT_MEMORY
static inline uint8_t LD8(uint64_t a) { return *(const uint8_t *)a; }
static inline uint16_t LD16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)a, 2); return v; }
static inline uint32_t LD32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)a, 4); return v; }
static inline uint64_t LD64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }
static inline void LDQ(vreg *r, uint64_t a) { memcpy(r, (const void *)a, 16); }
static inline void ST8(uint64_t a, uint8_t v) { *(uint8_t *)a = v; }
static inline void ST16(uint64_t a, uint16_t v) { memcpy((void *)a, &v, 2); }
static inline void ST32(uint64_t a, uint32_t v) { memcpy((void *)a, &v, 4); }
static inline void ST64(uint64_t a, uint64_t v) { memcpy((void *)a, &v, 8); }
static inline void STQ(uint64_t a, const vreg *r) { memcpy((void *)a, r, 16); }
// Acquire loads, release stores and compare-and-swaps: sequentially consistent (an stlr and a
// later ldar mustn't pass each other on arm64; a plain x86 store and load would).
#define LDA8(a) __atomic_load_n((uint8_t *)(a), __ATOMIC_SEQ_CST)
#define LDA16(a) __atomic_load_n((uint16_t *)(a), __ATOMIC_SEQ_CST)
#define LDA32(a) __atomic_load_n((uint32_t *)(a), __ATOMIC_SEQ_CST)
#define LDA64(a) __atomic_load_n((uint64_t *)(a), __ATOMIC_SEQ_CST)
#define STL8(a, v) __atomic_store_n((uint8_t *)(a), (v), __ATOMIC_SEQ_CST)
#define STL16(a, v) __atomic_store_n((uint16_t *)(a), (v), __ATOMIC_SEQ_CST)
#define STL32(a, v) __atomic_store_n((uint32_t *)(a), (v), __ATOMIC_SEQ_CST)
#define STL64(a, v) __atomic_store_n((uint64_t *)(a), (v), __ATOMIC_SEQ_CST)
// A compare-and-swap: whether *a was e (then it's v); the V forms leave what *a was in *e.
#define CAS8V(a, e, v) __atomic_compare_exchange_n((uint8_t *)(a), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define CAS16V(a, e, v) __atomic_compare_exchange_n((uint16_t *)(a), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define CAS32V(a, e, v) __atomic_compare_exchange_n((uint32_t *)(a), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define CAS64V(a, e, v) __atomic_compare_exchange_n((uint64_t *)(a), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define LDA128(a) ({ unsigned __int128 v_ = 0; __atomic_compare_exchange_n((unsigned __int128 *)(a), &v_, v_, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); v_; })
#define CAS128(a, e, v) ({ unsigned __int128 e_ = (e); __atomic_compare_exchange_n((unsigned __int128 *)(a), &e_, (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })
#define MEMCOPY(d, s, n) memmove((void *)(d), (const void *)(s), (n))
#define MEMSET(d, c, n) memset((void *)(d), (c), (n))
#endif
#define CAS_(T, a, e, v) ({ T e_ = (e); CAS##T(a, &e_, (v)); })
#define CASuint8_t CAS8V
#define CASuint16_t CAS16V
#define CASuint32_t CAS32V
#define CASuint64_t CAS64V
#define CAS8(a, e, v) CAS_(uint8_t, a, e, v)
#define CAS16(a, e, v) CAS_(uint16_t, a, e, v)
#define CAS32(a, e, v) CAS_(uint32_t, a, e, v)
#define CAS64(a, e, v) CAS_(uint64_t, a, e, v)
#define FLAGS_FCMP(a, b) do { double a_ = (a), b_ = (b); \
    if (a_ != a_ || b_ != b_) { n = 0; z = 0; cf = 1; vf = 1; } \
    else if (a_ == b_) { n = 0; z = 1; cf = 1; vf = 0; } \
    else if (a_ < b_) { n = 1; z = 0; cf = 0; vf = 0; } \
    else { n = 0; z = 0; cf = 1; vf = 0; } } while (0)

// Float to integer, arm64's way: NaN 0, out of range the nearest limit (x is already rounded).
static inline uint32_t CVT_I32(double x) { return x != x ? 0 : x >= 2147483648.0 ? 0x7fffffffu : x < -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)x; }
static inline uint64_t CVT_I64(double x) { return x != x ? 0 : x >= 9223372036854775808.0 ? 0x7fffffffffffffffull : x < -9223372036854775808.0 ? 0x8000000000000000ull : (uint64_t)(int64_t)x; }
static inline uint32_t CVT_U32(double x) { return x != x || x <= -1.0 ? 0 : x >= 4294967296.0 ? 0xffffffffu : (uint32_t)(x < 0 ? 0 : x); }
static inline uint64_t CVT_U64(double x) { return x != x || x <= -1.0 ? 0 : x >= 18446744073709551616.0 ? ~0ull : (uint64_t)(x < 0 ? 0 : x); }
// fmax, fmin (a NaN operand gives a NaN, +0 is above -0) and fmaxnm, fminnm (a NaN operand gives
// the other operand).
#define A64_MINMAX(T, S) \
  static inline T A64_FMAX##S(T a, T b) { if (a != a || b != b) return (a != a ? a : b) + 0; \
    if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; } \
  static inline T A64_FMIN##S(T a, T b) { if (a != a || b != b) return (a != a ? a : b) + 0; \
    if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; } \
  static inline T A64_FMAXNM##S(T a, T b) { if (a != a && b == b) return b; if (b != b && a == a) return a; return A64_FMAX##S(a, b); } \
  static inline T A64_FMINNM##S(T a, T b) { if (a != a && b == b) return b; if (b != b && a == a) return a; return A64_FMIN##S(a, b); }
A64_MINMAX(float, S)
A64_MINMAX(double, D)

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
    v22, v23, v24, v25, v26, v27, v28, v29, v30, v31; int n, z, cf, vf; uint64_t em_a = ~0ull, em_v = 0;     unsigned __int128 em_q = 0;

extern uint64_t BASE;  // the library's load address
void lift_fatal_assert(uint64_t file, uint64_t format, uint64_t arg);
void lift_fatal(const char *what, uint64_t at);
// The lifted function for a guest address (an indirect call's target), or NULL.
void (*lift_find(uint64_t address))(uint8_t *);
// Run the guest code at target on ts, translated, until it returns: to x30 (one of the table's stops),
// or (_stop) to the translator's own stop address.
void lift_guest_call(uint8_t *ts, uint64_t target);
void lift_guest_call_stop(uint8_t *ts, uint64_t target);
'''


TABLE = '''
struct prism_lifted {
  uint32_t offset;          // the guest function: its address from the library's base
  uint32_t nranges;         // its code: address ranges (offset, size pairs) from the base
  const uint32_t *ranges;
  uint64_t fnv;             // FNV-1a of those bytes, in order
  void (*lifted)(uint8_t *);
  const char *name;
  uint32_t nstops;          // its guest calls' return addresses (offsets), the translator's stops
  const uint32_t *stops;
};
'''


def lift_library(data, functions, externs, source=''):
    """C source for the functions ({address: (size or None, description)}) of the library in
    data; externs is {imported name or address: kind} (EXTERN_KINDS). Returns (source, lifter):
    the lifter's blocks are {function: [basic block addresses]}, for coverage checks, its
    guest_calls {function: call sites running guest code}."""
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
    lifter = Lifter(data, set(functions), by_addr)
    parts = [f'// Generated by tools/lift_arm64.py from {source} (build ID {build_id.hex()}); do not edit.', PRELUDE]
    parts += [f'void lift_{a:x}(uint8_t *ts);' for a in functions]
    for a in functions:
        parts.append(lifter.lift(a).replace('  LOAD_F;\n', '  DECL_ALL\n  LOAD_F;\n', 1))
    parts.append('#pragma clang attribute pop')
    # Code two lifted functions have (one running into the other) has its stops listed once: the
    # translator takes each once.
    owner = {}
    for a in functions:
        lifter.stops[a] = [r for r in lifter.stops[a] if owner.setdefault(r, a) == a]
    parts.append(TABLE)
    for a in functions:
        flat = ', '.join(f'{r:#x}, {n}' for r, n in lifter.ranges[a])
        parts.append(f'static const uint32_t ranges_{a:x}[] = {{{flat}}};')
        stops = ', '.join(f'{r:#x}' for r in sorted(set(lifter.stops[a]))) or '0'
        parts.append(f'static const uint32_t stops_{a:x}[] = {{{stops}}};')
    parts.append('const struct prism_lifted prism_lifted[] = {')
    for a, (_size, what) in functions.items():
        rs = lifter.ranges[a]
        code = b''.join(lifter.code(r, n) for r, n in rs)
        parts.append(f'    {{{a:#x}, {len(rs)}, ranges_{a:x}, {fnv1a(code):#018x}ull, lift_{a:x}, "lift_{a:x}", '
                     f'{len(set(lifter.stops[a]))}, stops_{a:x}}},  // {what}')
    parts.append('};')
    parts.append(f'const unsigned prism_lifted_count = {len(functions)};')
    parts.append(f'const uint8_t prism_lifted_build_id[{len(build_id)}] = {{{", ".join(f"{b:#04x}" for b in build_id)}}};')
    parts.append(f'const unsigned prism_lifted_build_id_size = {len(build_id)};\n')
    return '\n'.join(parts), lifter


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
        if mode is functions:
            k, _, v = a.partition('=')
            functions[int(k, 16)] = (int(v, 0) if v else None, '')
        else:
            k, v = a.split('=')
            externs[int(k, 16) if re.fullmatch(r'(0x)?[0-9a-fA-F]+', k) and not k.isalpha() else k] = v
    with open(lib, 'rb') as f:
        source, lifter = lift_library(f.read(), functions, externs, lib)
    with open(out, 'w', newline='\n') as f:
        f.write(source)
    with open(out + '.blocks', 'w') as f:
        for bl in lifter.blocks.values():
            f.write(' '.join(f'{b:x}' for b in bl) + '\n')


if __name__ == '__main__':
    main()
