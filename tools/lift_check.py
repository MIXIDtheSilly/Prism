"""Checks the lifted code (tools/lift_arm64.py, native/berberis_proxy_libshell) against VrShell's
own arm64 code, on Prism's emulator.

    python tools/lift_check.py insns [ADDR...] [--inputs 64]
    python tools/lift_check.py record [--timeout 300]
    python tools/lift_check.py replay [--fuzz 6]

insns: every distinct instruction of the lifted functions (and of the functions ADDR) the lifter
takes and that can run alone (no branches, calls or pc-relative addresses), each on --inputs
generated states (registers, flags, a window of memory its operand addresses; edge values: NaNs,
infinities, signed zeros, the integer limits), run as the instruction itself (check/insn_arm64.S,
translated) and as its lifted C (x86_64), comparing every register, the flags and the memory.
Differences only in a NaN's bits (its sign or payload: which operand's NaN an operation passes on,
and the sign of one it makes, differ between the two float units) are counted apart. Two known
differences are the translator's, the lifted code doing what arm64 does: vector ucvtf of 32-bit
lanes rounding the wrong way in some cases, fmaxnm/fminnm of +0 and -0.

record: builds the proxy with native/berberis_proxy_libshell/check/record.c, installs it over the
deployed one, restarts VrShell and has it record 6000 calls into the lifted code (every register
in and out, every memory access, the guest calls it makes with the registers they had and left, the
basic blocks reached) once you start using it: open app windows and move the controllers' rays over
them, the floor and the sky meanwhile. Then puts the deployed proxy back and restarts VrShell. The
recording goes to work/build/lift-check.

replay: runs each recorded call again through the original arm64 code (translated, on the
emulator) on the same registers and memory and compares what it leaves, byte for byte; a guest
call goes to a stub that checks the arm64 code made it with the same registers and gives back what
the call left (check/replay.c). With --fuzz N it also makes N variants of each call (check/fuzz.c:
floats and small integers changed), runs the lifted code on them and replays those too (but those
reaching a guest call). It then lists the basic blocks no call reached. Expected differences: a NaN
an operation creates from non-NaN operands has the sign bit set (x86's default NaN), and the blocks
after a fatal assert, which neither side can run here.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import emulator  # noqa: E402
import lift_arm64  # noqa: E402
import translator  # noqa: E402

CHECK = os.path.join(ROOT, 'native', 'berberis_proxy_libshell', 'check')
OUT = os.path.join(ROOT, 'work', 'build', 'lift-check')
DEVICE = '/data/local/tmp/prism-lift'
VRSHELL_FILES = '/data/data/com.oculus.vrshell/files'
PROXY = '/system/lib64/' + translator.PROXY_LIBSHELL
WARNINGS = ['-Wall', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-label',
            '-Wno-misleading-indentation']


def clang():
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    return os.path.join(translator.find_ndk(None), 'toolchains', 'llvm', 'prebuilt', host, 'bin',
                        'clang' + ('.exe' if os.name == 'nt' else ''))


def prepare():
    """libshell.so from the user's VrShell, and its lifted code; returns the library's bytes."""
    os.makedirs(OUT, exist_ok=True)
    with zipfile.ZipFile(translator.VRSHELL_APK) as apk:
        data = apk.read(translator.LIBSHELL)
    with open(os.path.join(OUT, 'libshell.so'), 'wb') as f:
        f.write(data)
    source, lifter = lift_arm64.lift_library(data, translator.libshell_functions(), translator.LIBSHELL_EXTERNS,
                                             f'VrShell.apk:{translator.LIBSHELL}')
    blocks = lifter.blocks
    with open(os.path.join(OUT, 'lifted_libshell.c'), 'w', newline='\n') as f:
        f.write(source)
    with open(os.path.join(OUT, 'blocks.json'), 'w') as f:
        json.dump({f'{a:x}': [f'{b:x}' for b in bl] for a, bl in blocks.items()}, f)
    return data


def build(output, sources, target='x86_64', flags=()):
    lib_dir = os.path.join(ROOT, 'work', 'build', 'translator', 'system', 'lib64')
    command = [clang(), f'--target={target}-linux-android34', '-O2', *WARNINGS, *flags, *sources, '-o', output]
    if output.endswith('.so'):
        command += ['-shared', '-fPIC', f'-Wl,-soname,{translator.PROXY_LIBSHELL}', f'-L{lib_dir}',
                    '-Wl,--no-as-needed', '-lberberis_arm64', '-Wl,--allow-shlib-undefined']
    subprocess.run(command + ['-llog', '-lm'], check=True)


def restart_vrshell():
    emulator.shell('kill $(pidof com.oculus.vrshell)', check=False)
    for _ in range(60):
        time.sleep(1)
        if emulator.shell('pidof com.oculus.vrshell', check=False).strip():
            return
    sys.exit('VrShell did not come back')


def record(args):
    prepare()
    lifted = os.path.join(OUT, 'lifted_libshell.c')
    recording_proxy = os.path.join(OUT, translator.PROXY_LIBSHELL)
    build(recording_proxy, [translator.PROXY_LIBSHELL_SOURCE, lifted, os.path.join(CHECK, 'record.c')],
          flags=['-DLIFT_RECORD', '-include', os.path.join(CHECK, 'record.h')])
    emulator.adb('remount')
    emulator.shell(f'mkdir -p {DEVICE}; cp {PROXY} {DEVICE}/deployed.so')
    try:
        emulator.adb('push', recording_proxy, PROXY)
        emulator.shell(f'chmod 644 {PROXY}; restorecon {PROXY}; setprop debug.prism.lift_record 0; '
                       f'rm -f {VRSHELL_FILES}/lift.rec {VRSHELL_FILES}/lift.cov')
        restart_vrshell()
        time.sleep(15)
        emulator.shell('setprop debug.prism.lift_record 1')
        print('recording: open app windows and move the rays over them, the floor and the sky', flush=True)
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if emulator.shell(f'ls {VRSHELL_FILES}/lift.cov', check=False).strip().endswith('lift.cov'):
                break
            time.sleep(2)
        else:
            sys.exit('VrShell recorded too few calls in time (are the rays moving over something?)')
        for name in ('lift.rec', 'lift.cov'):
            emulator.shell(f'cp {VRSHELL_FILES}/{name} {DEVICE}/{name}; chmod 644 {DEVICE}/{name}')
            emulator.adb('pull', f'{DEVICE}/{name}', os.path.join(OUT, name))
        print(f'recorded -> {os.path.join(OUT, "lift.rec")}')
    finally:
        emulator.shell(f'setprop debug.prism.lift_record 0; cp {DEVICE}/deployed.so {PROXY}; restorecon {PROXY}; '
                       f'rm -f {VRSHELL_FILES}/lift.rec {VRSHELL_FILES}/lift.cov')
        restart_vrshell()


def coverage(paths, blocks):
    reached = set()
    for path in paths:
        if os.path.exists(path):
            with open(path, 'rb') as f:
                data = f.read()
            reached |= set(struct.unpack(f'<{len(data) // 4}I', data))
    total = missing = 0
    for function, bl in blocks.items():
        lost = [b for b in bl if int(b, 16) not in reached]
        total += len(bl)
        missing += len(lost)
        print(f'  {function}: {len(bl) - len(lost)}/{len(bl)} blocks' + (f'; not reached: {" ".join(lost)}' if lost else ''))
    print(f'  {total - missing}/{total} basic blocks reached')


def replay(args):
    data = prepare()
    recording = os.path.join(OUT, 'lift.rec')
    if not os.path.exists(recording):
        sys.exit(f'no {recording}: run python tools/lift_check.py record first')
    lifted = os.path.join(OUT, 'lifted_libshell.c')
    build(os.path.join(OUT, 'replay'), [os.path.join(CHECK, 'replay.c'), os.path.join(CHECK, 'call.S')], target='aarch64')
    build(os.path.join(OUT, 'fuzz'), [os.path.join(CHECK, 'fuzz.c'), lifted, os.path.join(CHECK, 'record.c')],
          flags=['-DFUZZ', '-include', os.path.join(CHECK, 'record.h')])
    imports = lift_arm64.Elf(data).imports()
    got = [f'{name}={imports[name][1]:x}' for name in translator.LIBSHELL_EXTERNS if isinstance(name, str)]
    emulator.shell(f'mkdir -p {DEVICE}')
    for name in ('replay', 'fuzz', 'libshell.so', 'lift.rec'):
        emulator.adb('push', os.path.join(OUT, name), f'{DEVICE}/{name}')
    emulator.shell(f'chmod 755 {DEVICE}/replay {DEVICE}/fuzz')
    run = lambda command: print(emulator.shell(f'cd {DEVICE} && {command} 2>&1', check=False).rstrip())
    print('VrShell\'s calls:')
    run(f'./replay libshell.so lift.rec 0 {" ".join(got)}')
    covers = [os.path.join(OUT, 'lift.cov')]
    if args.fuzz:
        print(f'{args.fuzz} variants of each:')
        run(f'./fuzz libshell.so lift.rec fuzz.rec {args.fuzz} {int(time.time())} fuzz.cov')
        run(f'./replay libshell.so fuzz.rec 0 {" ".join(got)}')
        emulator.adb('pull', f'{DEVICE}/fuzz.cov', os.path.join(OUT, 'fuzz.cov'))
        emulator.shell(f'rm -f {DEVICE}/fuzz.rec')
        covers.append(os.path.join(OUT, 'fuzz.cov'))
    with open(os.path.join(OUT, 'blocks.json')) as f:
        blocks = json.load(f)
    print('coverage:')
    coverage(covers, blocks)


INSN_SKIP = ('b', 'bl', 'blr', 'br', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'brk', 'udf', 'hlt', 'adr', 'adrp', 'mrs',
             'nop', 'prfm', 'prfum', 'hint', 'bti', 'paciasp', 'autiasp', 'pacibsp', 'autibsp')


def insn_tests(data, functions):
    """The distinct instructions of the functions (each one's .eh_frame range) the lifter takes and
    a test can run alone: no branches, calls, pc-relative addresses, x16 or x18. Returns
    [(capstone insn, word, C, (base, index, offset))]."""
    lifter = lift_arm64.Lifter(data, set(), {})
    ranges = lift_arm64.Elf(data).functions()
    seen, tests = set(), []
    for f in functions:
        # Its .eh_frame range, and the code it reaches beyond (a shared tail, a tail call's), if it lifts.
        spans = [(f, ranges[f])] if f in ranges else []
        try:
            lifter.lift(f)
            spans += lifter.ranges[f]
        except lift_arm64.Unsupported:
            pass
        if not spans:
            print(f'  {f:#x}: not a function start; skipped')
            continue
        for i in (i for start, size in spans for i in lifter.md.disasm(lifter.code(start, size), start)):
            word, = struct.unpack('<I', i.bytes)
            if word in seen or i.mnemonic in INSN_SKIP or i.mnemonic.startswith('b.'):
                continue
            seen.add(word)
            ops = lift_arm64.split_ops(i.op_str)
            if any(o.startswith('#') for o in ops[1:2]) and i.mnemonic.startswith(('ldr', 'ldrsw')) and '[' not in i.op_str:
                continue  # a literal: pc-relative
            if re.search(r'\b[xw](16|18)\b', i.op_str):
                continue
            try:
                c = lifter.insn(i, word)
            except (lift_arm64.Unsupported, ValueError, KeyError, IndexError):
                continue
            meta = (-1, -1, 0)
            m = re.search(r'\[((?:x\d+|sp)[^\]]*)\]', i.op_str)
            if m:
                parts = [p.strip() for p in m.group(1).split(',')]
                base = 31 if parts[0] == 'sp' else int(parts[0][1:])
                index, offset = -1, 0
                if len(parts) > 1 and parts[1].startswith('#'):
                    offset = lift_arm64.imm(parts[1])
                elif len(parts) > 1:
                    index = int(parts[1][1:])
                if abs(offset) > 0x10000:
                    continue
                meta = (base, index, offset)
            tests.append((i, word, c, meta))
    return tests


def insns(args):
    """Each instruction alone, arm64 (translated) against lifted (x86_64)."""
    data = prepare()
    functions = list(translator.libshell_functions()) + [int(a, 16) for a in args.functions]
    tests = insn_tests(data, functions)
    work = os.path.join(OUT, 'insns')
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'insn_meta.h'), 'w', newline='\n') as f:
        f.write(f'enum {{ NTESTS = {len(tests)} }};\nstatic const struct meta g_meta[NTESTS] = {{\n')
        for i, word, c, (b, x, o) in tests:
            f.write(f'  {{{b}, {x}, {o}}},  // {i.mnemonic} {i.op_str}\n')
        f.write('};\n')
    with open(os.path.join(work, 'insn_table.S'), 'w', newline='\n') as f:
        f.write('    .text\n    .p2align 4\n    .global insn_table\ninsn_table:\n')
        for i, word, c, meta in tests:
            f.write(f'    clrex\n    .inst {word:#010x}  // {i.mnemonic} {i.op_str}\n    br x18\n    nop\n')
    with open(os.path.join(work, 'insn_x86.c'), 'w', newline='\n') as f:
        f.write(lift_arm64.PRELUDE)
        f.write('uint64_t BASE;\nvoid lift_fatal_assert(uint64_t a, uint64_t b, uint64_t c) { (void)a, (void)b, (void)c; __builtin_trap(); }\n'
                'void lift_fatal(const char *w, uint64_t a) { (void)w, (void)a; __builtin_trap(); }\n'
                'void lift_guest_call(uint8_t *ts, uint64_t t) { (void)ts, (void)t; __builtin_trap(); }\n')
        for k, (i, word, c, meta) in enumerate(tests):
            f.write(f'static void t_{k}(uint8_t *ts) {{ DECL_ALL LOAD_ALL; uint64_t f_ = *(uint64_t *)(ts + 0x400); '
                    f'n = f_ >> 31 & 1; z = f_ >> 30 & 1; cf = f_ >> 29 & 1; vf = f_ >> 28 & 1;\n'
                    f'  /* {i.mnemonic} {i.op_str} */ {c}\n'
                    f'  STORE_ALL; *(uint64_t *)(ts + 0x400) = (uint64_t)n << 31 | (uint64_t)z << 30 | (uint64_t)cf << 29 | (uint64_t)vf << 28; }}\n')
        f.write('#pragma clang attribute pop\n')
        f.write('struct tstate { uint64_t x[31]; uint64_t sp; uint64_t nzcv; uint8_t v[32][16]; };\n')
        f.write('static void (*const tests[])(uint8_t *) = {' + ', '.join(f't_{k}' for k in range(len(tests))) + '};\n')
        f.write('int run_test(int k, const struct tstate *in, struct tstate *out) {\n'
                '  _Alignas(16) uint8_t ts[0x500] = {0};\n'
                '  memcpy(ts, in->x, 248); memcpy(ts + 0x108, &in->sp, 8); memcpy(ts + 0x110, in->v, 512); memcpy(ts + 0x400, &in->nzcv, 8);\n'
                '  tests[k](ts);\n'
                '  memcpy(out->x, ts, 248); memcpy(&out->sp, ts + 0x108, 8); memcpy(out->v, ts + 0x110, 512); memcpy(&out->nzcv, ts + 0x400, 8);\n'
                '  return 0;\n}\n')
    print(f'{len(tests)} instructions from {len(functions)} functions')
    source = os.path.join(CHECK, 'insn_test.c')
    build(os.path.join(work, 'insn_arm64'), [source, os.path.join(CHECK, 'insn_arm64.S')], target='aarch64', flags=[f'-I{work}'])
    build(os.path.join(work, 'insn_x86'), [source, os.path.join(work, 'insn_x86.c')], flags=[f'-I{work}', '-O1'])
    emulator.shell(f'mkdir -p {DEVICE}')
    for name in ('insn_arm64', 'insn_x86'):
        emulator.adb('push', os.path.join(work, name), f'{DEVICE}/{name}')
    emulator.shell(f'chmod 755 {DEVICE}/insn_arm64 {DEVICE}/insn_x86')
    for name in ('insn_arm64', 'insn_x86'):
        start = time.time()
        print(emulator.shell(f'cd {DEVICE} && ./{name} {name}.bin {args.inputs} 2>&1', check=False).rstrip(),
              end='')
        print(f'{name}: {time.time() - start:.0f} s')
        emulator.adb('pull', f'{DEVICE}/{name}.bin', os.path.join(work, f'{name}.bin'))
    hashes = {}
    for name in ('insn_arm64', 'insn_x86'):
        with open(os.path.join(work, f'{name}.bin'), 'rb') as f:
            raw = f.read()
        hashes[name] = struct.unpack(f'<{len(raw) // 8}Q', raw)
    a, x = hashes['insn_arm64'], hashes['insn_x86']
    failing, nan_only = {}, []
    for k in range(len(tests)):
        at = 2 * k * args.inputs
        bad = [j for j in range(args.inputs) if a[at + 2 * j] != x[at + 2 * j]]
        # A NaN's sign or payload (both sides NaN) is no difference code sees but in its bits.
        real = [j for j in bad if a[at + 2 * j + 1] != x[at + 2 * j + 1]]
        if real:
            failing[k] = real
        elif bad:
            nan_only.append(k)
    print(f'{len(tests) - len(failing) - len(nan_only)} of {len(tests)} instructions the same on all {args.inputs} '
          f'inputs, {len(nan_only)} but for a NaN\'s bits, {len(failing)} different:')
    shown = 0
    for k, bad in failing.items():
        i, word, c, meta = tests[k]
        shown += 1
        if shown <= args.show:
            reasons = [explain(k, j)[0] for j in bad[:3]]
            print(f'  {i.mnemonic} {i.op_str} ({word:#010x}, {len(bad)}/{args.inputs} inputs): {"; ".join(reasons)}')
    if shown > args.show:
        print(f'  ... and {shown - args.show} more')


def explain(k, j):
    """What differs between the two sides for test k, input j."""
    out = {}
    for name in ('insn_arm64', 'insn_x86'):
        path = os.path.join(OUT, 'insns', f'{name}.dump')
        if os.path.exists(path):
            os.remove(path)
        emulator.shell(f'cd {DEVICE} && rm -f {name}.dump && ./{name} --dump {k} {j} {name}.dump', check=False)
        try:
            emulator.adb('pull', f'{DEVICE}/{name}.dump', path)
        except RuntimeError:
            return f'{name} crashed on input {j}', False
        with open(path, 'rb') as f:
            out[name] = f.read()
    a, x = out['insn_arm64'], out['insn_x86']
    fa, fx = struct.unpack_from('<i', a)[0], struct.unpack_from('<i', x)[0]
    if fa or fx:
        return f'faulted: arm64 {fa}, lifted {fx}', False
    sa, sx = a[4:4 + 776], x[4:4 + 776]
    diffs = []
    for r in range(31):
        if r in (16, 18):
            continue
        va, vx = struct.unpack_from('<Q', sa, 8 * r)[0], struct.unpack_from('<Q', sx, 8 * r)[0]
        if va != vx:
            diffs.append(f'x{r} {va:#x}/{vx:#x}')
    for name, off in (('sp', 248), ('nzcv', 256)):
        va, vx = struct.unpack_from('<Q', sa, off)[0], struct.unpack_from('<Q', sx, off)[0]
        if name == 'nzcv':
            va, vx = va >> 28, vx >> 28
        if va != vx:
            diffs.append(f'{name} {va:#x}/{vx:#x}')
    for r in range(32):
        va, vx = sa[264 + 16 * r:280 + 16 * r], sx[264 + 16 * r:280 + 16 * r]
        if va != vx:
            wa, wx = struct.unpack('<4I', va), struct.unpack('<4I', vx)
            nan = all(p == q or ((p & 0x7f800000) == 0x7f800000 and (p & 0x7fffff) and (q & 0x7f800000) == 0x7f800000 and (q & 0x7fffff))
                      for p, q in zip(wa, wx))
            diffs.append(f'v{r}{" (NaNs)" if nan else ""} {" ".join(f"{w:08x}" for w in wa)}/{" ".join(f"{w:08x}" for w in wx)}')
    ma, mx = a[780:780 + 1024], x[780:780 + 1024]
    if ma != mx:
        at = next(n for n in range(1024) if ma[n] != mx[n])
        diffs.append(f'memory +{at - 512} {ma[at:at + 8].hex()}/{mx[at:at + 8].hex()}')
    ins = a[780 + 1024:780 + 1024 + 776]
    regs = ' '.join(f'x{r}={struct.unpack_from("<Q", ins, 8 * r)[0]:#x}' for r in range(8))
    nan_only = all('(NaNs)' in d for d in diffs)
    return ', '.join(diffs) + f' [in {regs} nzcv={struct.unpack_from("<Q", ins, 256)[0] >> 28:#x}]', nan_only


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    r = sub.add_parser('record')
    r.add_argument('--timeout', type=int, default=300, help='seconds to wait for 6000 calls')
    p = sub.add_parser('replay')
    p.add_argument('--fuzz', type=int, default=6, help='variants of each recorded call (0: none)')
    t = sub.add_parser('insns')
    t.add_argument('functions', nargs='*', help='more functions (hex addresses) whose instructions to test')
    t.add_argument('--inputs', type=int, default=64, help='states each instruction runs on')
    t.add_argument('--show', type=int, default=40, help='instructions to explain that differ')
    args = ap.parse_args()
    {'record': record, 'replay': replay, 'insns': insns}[args.command](args)


if __name__ == '__main__':
    main()
