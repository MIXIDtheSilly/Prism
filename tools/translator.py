"""Prepare the Digitalis ARM64 translator (prebuilts/digitalis) for Prism's Android 14 base.

Digitalis is built for Android 16. Its host binaries import a few symbols Android 14's x86_64
libraries lack (see native/berberis_compat). This builds that shim (libpcx.so), copies the
bundle to work/build/translator/, points each host binary's DT_NEEDED "libc++.so" at the shim
(same length, so the change is in place), and checks that every host import then resolves
against the stock image. It also disables the guest libdl's CFI slow path (see disable_guest_cfi)
and keeps block chaining's per-thread state usable through thread exit (see keep_chain_sites).

    python tools/translator.py [--ndk PATH]
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import extracted, find_ndk  # noqa: E402
import stamp  # noqa: E402

BUNDLE = os.path.join(ROOT, 'prebuilts', 'digitalis')
SHIM_SOURCE = os.path.join(ROOT, 'native', 'berberis_compat', 'berberis_compat.c')
BIONIC = os.path.join(ROOT, 'work', 'device', 'bionic')


def inputs(bionic=BIONIC):
    """The bundle, the shim, and what imports are checked against: stock's libraries and bionic."""
    return [BUNDLE, SHIM_SOURCE, PROXY_LIBM_SOURCE, __file__, bionic, *extracted(os.path.join(ROOT, 'work', 'baseline', 'fs'))]
SHIM = 'libpcx.so'
REPLACED = 'libc++.so'
assert len(SHIM) == len(REPLACED)
DT_NEEDED, DT_STRTAB, DT_VERNEED, DT_VERNEEDNUM = 1, 5, 0x6FFFFFFE, 0x6FFFFFFF


class Elf:
    """Just enough of a 64-bit ELF to read and edit its dynamic section."""

    def __init__(self, data):
        self.data = bytearray(data)
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
        self.loads, self.dynamic = [], None
        for i in range(phnum):
            p_type, _flags, offset, vaddr, _paddr, filesz = struct.unpack_from('<IIQQQQ', data, phoff + i * phentsize)
            if p_type == 1:
                self.loads.append((vaddr, offset, filesz))
            elif p_type == 2:
                self.dynamic = (offset, filesz)
        self.entries = []
        for pos in range(self.dynamic[0], self.dynamic[0] + self.dynamic[1], 16):
            tag, value = struct.unpack_from('<qQ', data, pos)
            if tag == 0:
                break
            self.entries.append((tag, value))
        self.strtab = self.offset(self.value(DT_STRTAB))

    def value(self, tag):
        return next((v for t, v in self.entries if t == tag), None)

    def offset(self, vaddr):
        return next(off + vaddr - va for va, off, size in self.loads if va <= vaddr < va + size)

    def string(self, index):
        start = self.strtab + index
        return self.data[start:self.data.index(0, start)].decode()

    def needed(self):
        return [(v, self.string(v)) for t, v in self.entries if t == DT_NEEDED]

    def verneed_files(self):
        out, pos = set(), self.value(DT_VERNEED)
        if pos is None:
            return out
        pos = self.offset(pos)
        for _ in range(self.value(DT_VERNEEDNUM) or 0):
            _version, _count, file, _aux, nxt = struct.unpack_from('<HHIII', self.data, pos)
            out.add(file)
            if not nxt:
                break
            pos += nxt
        return out

    def rename_needed(self, old, new):
        for index, name in self.needed():
            if name == old:
                if index in self.verneed_files():
                    raise ValueError(f'{old} is also named by a version requirement')
                start = self.strtab + index
                self.data[start:start + len(new)] = new.encode()
                return True
        return False


def dynamic_symbols(path):
    """(undefined, defined) dynamic symbol names; weak undefined ones are optional and skipped."""
    with open(path, 'rb') as f:
        b = f.read()
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', b, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next(s for s in sections if s[1] == 11)
    dynstr = sections[dynsym[6]]
    undefined, defined = set(), set()
    for i in range(1, dynsym[5] // 24):
        name, info, _other, shndx, _value, _size = struct.unpack_from('<IBBHQQ', b, dynsym[4] + i * 24)
        text = b[dynstr[4] + name:b.index(b'\0', dynstr[4] + name)].decode()
        if shndx == 0 and info >> 4 == 2:
            continue
        (undefined if shndx == 0 else defined).add(text)
    return undefined, defined


def symbol_values(data):
    """Defined dynamic symbols -> their addresses (names without version suffixes)."""
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next(s for s in sections if s[1] == 11)
    dynstr = sections[dynsym[6]]
    out = {}
    for i in range(1, dynsym[5] // 24):
        name, _info, _other, shndx, value, _size = struct.unpack_from('<IBBHQQ', data, dynsym[4] + i * 24)
        if shndx:
            out[data[dynstr[4] + name:data.index(b'\0', dynstr[4] + name)].decode()] = value
    return out


# Horizon's platform libraries are built with cross-library CFI. The bionic linker keeps a shadow
# map of valid call targets for its checks, but the translator's guest loader never records guest
# libraries there, so a check that reaches the slow path reads an unmapped shadow page and the
# process dies. Prism turns the guest slow path into a return: CFI is hardening, not function.
GUEST_LIBDL = os.path.join('system', 'lib64', 'arm64', 'libdl.so')
CFI_SLOWPATHS = ('__cfi_slowpath', '__cfi_slowpath_diag')
ARM64_RET = struct.pack('<I', 0xd65f03c0)


def disable_guest_cfi(out_dir):
    path = os.path.join(out_dir, GUEST_LIBDL)
    with open(path, 'rb') as f:
        elf = Elf(f.read())
    values = symbol_values(bytes(elf.data))
    for name in CFI_SLOWPATHS:
        if name not in values:
            sys.exit(f'{GUEST_LIBDL} has no {name}')
        start = elf.offset(values[name])
        elf.data[start:start + 4] = ARM64_RET
    with open(path, 'wb') as f:
        f.write(elf.data)


# The bundle's block chaining (prebuilts/digitalis/MANIFEST.txt) keeps the chainable jumps of the
# code just installed in a thread_local vector (berberis::LastInstalledChainSites). bionic destroys
# a thread's thread_locals before it runs its pthread key destructors, and an arm64 key destructor
# can reach code not yet translated: InstallTranslated then reuses the destroyed vector, and growing
# it frees its old buffer a second time (scudo: "invalid chunk state when deallocating"; VrShell,
# the runtime service and others died of it). The destructor frees the buffer but leaves the vector
# pointing at it. Two places register it (the accessor, and its copy inlined in InstallTranslated),
# so Prism makes the destructor itself return at once: the vector stays usable to the end of the
# thread, and its buffer is left behind when the thread ends.
CHAIN_SITES = '_ZN8berberis23LastInstalledChainSitesEv'
CHAIN_SITES_DTOR = bytes.fromhex('4889f8488b3f4885ff')  # mov rax, rdi; mov rdi, [rdi]; test rdi, rdi
X86_RET = bytes.fromhex('c3')
X86_LEA_RDI = bytes.fromhex('488d3d')  # lea rdi, [rip + d]


def keep_chain_sites(out_dir):
    path = os.path.join(out_dir, 'system', 'lib64', 'libberberis_arm64.so')
    with open(path, 'rb') as f:
        elf = Elf(f.read())
    data = bytes(elf.data)
    values = symbol_values(data)
    if CHAIN_SITES not in values:
        return False  # a build without block chaining
    start = values[CHAIN_SITES]
    # The accessor passes the destructor to __cxa_thread_atexit in rdi; it's the only rip-relative
    # lea into rdi there that points just past the accessor (the other is its TLS descriptor).
    dtors = set()
    for at in range(start, start + 0x60):
        o = elf.offset(at)
        if data[o:o + 3] == X86_LEA_RDI:
            target = at + 7 + struct.unpack_from('<i', data, o + 3)[0]
            if start < target < start + 0x100:
                dtors.add(target)
    if len(dtors) != 1:
        sys.exit(f'libberberis_arm64.so: expected one destructor in {CHAIN_SITES}, found {len(dtors)}')
    at = elf.offset(dtors.pop())
    if data[at:at + len(CHAIN_SITES_DTOR)] != CHAIN_SITES_DTOR:
        sys.exit('libberberis_arm64.so: the chain sites destructor is not as expected')
    elf.data[at:at + 1] = X86_RET
    with open(path, 'wb') as f:
        f.write(elf.data)
    return True


# Math through the PC's libm (native/berberis_proxy_libm): Digitalis proxies only expf and powf. A
# proxied function takes two halves: the host proxy's table, and the guest library handing the
# function's address to the translator at load (native_bridge_intercept_symbol(address, "libm.so",
# name), from an init function, which Digitalis's guest libm does for its two). Prism builds the
# proxy, and gives the guest libm a new init function that calls the old one, then intercepts every
# other function the proxy lists at its own address; the code and names go in the zero padding after
# the guest libm's code segment, which grows over them.
PROXY_LIBM_SOURCE = os.path.join(ROOT, 'native', 'berberis_proxy_libm', 'proxy_libm.c')
PROXY_LIBM = 'libberberis_proxy_libm.so'
GUEST_LIBM = os.path.join('system', 'lib64', 'arm64', 'libm.so')
INTERCEPT = 'native_bridge_intercept_symbol'
R_AARCH64_GLOB_DAT, R_AARCH64_RELATIVE = 1025, 1027
PT_LOAD, PF_X = 1, 1


def proxy_names():
    """The functions the proxy lists, from its table."""
    with open(PROXY_LIBM_SOURCE) as f:
        source = f.read()
    table = source[source.index('kTrampolines[] = {'):]
    table = table[:table.index('};')]
    return [a or b for a, b in re.findall(r'\b(?:F|FF|D|DD)\((\w+)\)|\{"(\w+)"', table)]


def arm64_adr(pc, target, rd):
    d = target - pc
    assert -(1 << 20) <= d < (1 << 20)
    return struct.pack('<I', 0x10000000 | (d & 3) << 29 | ((d >> 2) & 0x7FFFF) << 5 | rd)


def arm64_bl(pc, target):
    return struct.pack('<I', 0x94000000 | ((target - pc) >> 2) & 0x3FFFFFF)


def intercept_more_libm(out_dir, names):
    path = os.path.join(out_dir, GUEST_LIBM)
    with open(path, 'rb') as f:
        elf = Elf(f.read())
    data = elf.data
    values = symbol_values(bytes(data))
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    strtab = sections[shstrndx][4]
    section = {data[strtab + s[0]:data.index(b'\0', strtab + s[0])].decode(): s for s in sections}
    dynsym, dynstr = section['.dynsym'], section['.dynstr']

    def symbol_name(index):
        name, = struct.unpack_from('<I', data, dynsym[4] + index * 24)
        return data[dynstr[4] + name:data.index(b'\0', dynstr[4] + name)].decode()

    # The init function: .init_array's one entry, a relative relocation.
    init_array = section['.init_array']
    rela = section['.rela.dyn']
    slots = [rela[4] + i for i in range(0, rela[5], 24)
             if struct.unpack_from('<QQ', data, rela[4] + i) == (init_array[3], R_AARCH64_RELATIVE)]
    if init_array[5] != 8 or len(slots) != 1:
        sys.exit(f'{GUEST_LIBM}: expected one init function')
    old_init, = struct.unpack_from('<q', data, slots[0] + 16)
    # The intercept call's PLT entry: the init function's first bl, whose GOT slot is the import's.
    at = elf.offset(old_init)
    for i in range(0, 0x100, 4):
        w, = struct.unpack_from('<I', data, at + i)
        if w & 0xFC000000 == 0x94000000:
            plt = old_init + i + ((w & 0x3FFFFFF) << 2 if not w & 0x2000000 else ((w & 0x3FFFFFF) - (1 << 26)) << 2)
            break
    else:
        sys.exit(f'{GUEST_LIBM}: its init function calls nothing')
    adrp, ldr = struct.unpack_from('<II', data, elf.offset(plt))
    page = ((adrp >> 29 & 3 | (adrp >> 5 & 0x7FFFF) << 2) ^ 1 << 20) - (1 << 20)
    got = (plt & ~0xFFF) + (page << 12) + ((ldr >> 10 & 0xFFF) << 3)
    relplt = section['.rela.plt']
    imports = {struct.unpack_from('<Q', data, relplt[4] + i)[0]:
               symbol_name(struct.unpack_from('<Q', data, relplt[4] + i + 8)[0] >> 32) for i in range(0, relplt[5], 24)}
    if imports.get(got) != INTERCEPT:
        sys.exit(f'{GUEST_LIBM}: its init function does not start with {INTERCEPT}')
    # The functions it intercepts already: the addresses it loads from the GOT (adrp, then ldr).
    got_symbols = {struct.unpack_from('<Q', data, rela[4] + i)[0]:
                   symbol_name(struct.unpack_from('<Q', data, rela[4] + i + 8)[0] >> 32) for i in range(0, rela[5], 24)
                   if struct.unpack_from('<Q', data, rela[4] + i + 8)[0] & 0xFFFFFFFF == R_AARCH64_GLOB_DAT}
    done, pages = set(), {}
    for i in range(0, 0x100, 4):
        w, = struct.unpack_from('<I', data, at + i)
        if w & 0x9F000000 == 0x90000000:  # adrp
            imm = ((w >> 29 & 3 | (w >> 5 & 0x7FFFF) << 2) ^ 1 << 20) - (1 << 20)
            pages[w & 31] = ((old_init + i) & ~0xFFF) + (imm << 12)
        elif w & 0xFFC00000 == 0xF9400000 and (w >> 5 & 31) in pages:  # ldr xt, [xn, #imm]
            name = got_symbols.get(pages[w >> 5 & 31] + ((w >> 10 & 0xFFF) << 3))
            if name:
                done.add(name)
        elif w == 0xD65F03C0:
            break
    if not done:
        sys.exit(f'{GUEST_LIBM}: its init function intercepts nothing')
    library = data.find(b'\0libm.so\0') + 1
    if library <= 0 or elf.offset(library) != library:
        sys.exit(f'{GUEST_LIBM}: no "libm.so"')
    missing = [n for n in names if n not in values]
    if missing:
        sys.exit(f'{GUEST_LIBM}: no {", ".join(missing)}')
    names = [n for n in names if n not in done]

    # The code segment, and the padding after it.
    for i in range(phnum):
        h = phoff + i * phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<IIQQQQQ', data, h)
        if p_type == PT_LOAD and p_flags & PF_X:
            break
    else:
        sys.exit(f'{GUEST_LIBM}: no code segment')
    next_offset = min(struct.unpack_from('<Q', data, phoff + j * phentsize + 8)[0] for j in range(phnum)
                      if struct.unpack_from('<I', data, phoff + j * phentsize)[0] == PT_LOAD
                      and struct.unpack_from('<Q', data, phoff + j * phentsize + 8)[0] > p_offset)
    start = (p_vaddr + p_filesz + 15) & ~15
    code_size = 4 * (3 + 4 * len(names) + 2)
    strings = start + code_size
    end = strings + sum(len(n) + 1 for n in names)
    if p_offset != p_vaddr or end - p_vaddr > next_offset - p_offset or any(data[p_offset + p_filesz:end]):
        sys.exit(f'{GUEST_LIBM}: no room after its code')
    code = bytearray()
    pc = lambda: start + len(code)
    code += struct.pack('<II', 0xA9BF7BFD, 0x910003FD)  # stp x29, x30, [sp, #-16]!; mov x29, sp
    code += arm64_bl(pc(), old_init)
    at_name = strings
    for name in names:
        code += arm64_adr(pc(), values[name], 0)
        code += arm64_adr(pc(), library, 1)
        code += arm64_adr(pc(), at_name, 2)
        code += arm64_bl(pc(), plt)
        at_name += len(name) + 1
    code += struct.pack('<II', 0xA8C17BFD, 0xD65F03C0)  # ldp x29, x30, [sp], #16; ret
    assert len(code) == code_size
    blob = bytes(code) + b''.join(n.encode() + b'\0' for n in names)
    data[start:end] = blob
    struct.pack_into('<QQ', data, h + 32, end - p_vaddr, end - p_vaddr)
    struct.pack_into('<q', data, slots[0] + 16, start)
    with open(path, 'wb') as f:
        f.write(data)
    return len(names)


def build_proxy_libm(ndk, lib_dir):
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    out = os.path.join(lib_dir, PROXY_LIBM)
    subprocess.run([clang, '--target=x86_64-linux-android34', '-shared', '-fPIC', '-O2', '-Wall', PROXY_LIBM_SOURCE,
                    '-o', out, f'-Wl,-soname,{PROXY_LIBM}', f'-L{lib_dir}', '-Wl,--no-as-needed', '-lberberis_arm64',
                    '-lm', '-Wl,--allow-shlib-undefined', '-Wl,--build-id=sha1'], check=True)
    return out


def build_shim(ndk, out_dir):
    host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
    stub_dir = tempfile.mkdtemp(prefix='prism-libcxx-')
    try:
        # Link against an empty libc++.so so the shim gets DT_NEEDED libc++.so (the platform's).
        stub_src = os.path.join(stub_dir, 'empty.c')
        with open(stub_src, 'w') as f:
            f.write('')
        subprocess.run([clang, '--target=x86_64-linux-android34', '-shared', '-nostdlib', stub_src,
                        '-o', os.path.join(stub_dir, 'libc++.so'), '-Wl,-soname,libc++.so'], check=True)
        out = os.path.join(out_dir, SHIM)
        subprocess.run([clang, '--target=x86_64-linux-android34', '-shared', '-fPIC', '-O2', '-Wall',
                        SHIM_SOURCE, '-o', out, f'-Wl,-soname,{SHIM}', f'-L{stub_dir}',
                        '-Wl,--no-as-needed', '-lc++', '-llog', '-Wl,--build-id=sha1'], check=True)
    finally:
        shutil.rmtree(stub_dir, ignore_errors=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ndk')
    ap.add_argument('--stock', default=os.path.join('work', 'baseline', 'fs', 'system', 'system', 'lib64'),
                    help='stock x86_64 /system/lib64, to check imports against')
    ap.add_argument('--bionic', default=os.path.join('work', 'device', 'bionic'),
                    help='stock bionic from /apex/com.android.runtime (pulled from the emulator)')
    ap.add_argument('--out', default=os.path.join('work', 'build', 'translator'))
    ap.add_argument('--force', action='store_true', help='build even if current')
    args = ap.parse_args()
    fingerprint = stamp.fingerprint(inputs(args.bionic))
    if stamp.up_to_date(args.out, fingerprint, args.force):
        return

    with open(os.path.join(BUNDLE, 'SHA256SUMS')) as f:
        for line in f:
            digest, rel = line.split()
            with open(os.path.join(BUNDLE, rel), 'rb') as g:
                if hashlib.sha256(g.read()).hexdigest() != digest:
                    sys.exit(f'{rel}: does not match prebuilts/digitalis/SHA256SUMS')
    shutil.rmtree(args.out, ignore_errors=True)
    shutil.copytree(os.path.join(BUNDLE, 'system'), os.path.join(args.out, 'system'))
    disable_guest_cfi(args.out)
    keep_chain_sites(args.out)
    ndk = find_ndk(args.ndk)
    shim = build_shim(ndk, os.path.join(args.out, 'system', 'lib64'))
    build_proxy_libm(ndk, os.path.join(args.out, 'system', 'lib64'))
    intercepted = intercept_more_libm(args.out, proxy_names())
    shim_symbols = dynamic_symbols(shim)[1]

    hosts = sorted(glob.glob(os.path.join(args.out, 'system', 'lib64', 'libberberis_*.so')) +
                   glob.glob(os.path.join(args.out, 'system', 'bin', 'berberis_*')))
    patched = 0
    for path in hosts:
        if dynamic_symbols(path)[0] & shim_symbols:
            with open(path, 'rb') as f:
                elf = Elf(f.read())
            if not elf.rename_needed(REPLACED, SHIM):
                sys.exit(f'{os.path.basename(path)} needs a shim symbol but has no DT_NEEDED {REPLACED}')
            with open(path, 'wb') as f:
                f.write(elf.data)
            patched += 1

    # The linker resolves a library's imports only from its own DT_NEEDED closure. Libraries that
    # live in APEXes (libnativehelper, libicu, ...) aren't in these directories and are skipped.
    search = [os.path.join(args.out, 'system', 'lib64'), args.bionic, args.stock]
    locate = lambda name: next((p for p in (os.path.join(d, name) for d in search) if os.path.exists(p)), None)
    cache, unresolved = {}, {}

    def info(p):
        if p not in cache:
            with open(p, 'rb') as f:
                cache[p] = ([n for _, n in Elf(f.read()).needed()], dynamic_symbols(p))
        return cache[p]

    for path in hosts + [shim]:
        seen, queue, provided = set(), [path], set()
        while queue:
            p = queue.pop(0)
            if p in seen:
                continue
            seen.add(p)
            needed, (_undefined, defined) = info(p)
            provided |= defined
            queue += [q for q in map(locate, needed) if q]
        missing = info(path)[1][0] - provided
        if missing:
            unresolved[os.path.basename(path)] = sorted(missing)
    if unresolved:
        sys.exit(f'imports with no provider on Android 14: {unresolved}')
    stamp.write(args.out, fingerprint)
    print(f'{len(hosts)} host binaries, {patched} pointed at {SHIM}; {intercepted} more libm functions proxied; '
          f'all imports resolve -> {args.out}')


if __name__ == '__main__':
    main()
