"""Prepare the Digitalis ARM64 translator (prebuilts/digitalis) for Prism's Android 14 base.

Digitalis is built for Android 16. Its host binaries import a few symbols Android 14's x86_64
libraries lack (see native/berberis_compat). This builds that shim (libpcx.so), copies the
bundle to work/build/translator/, points each host binary's DT_NEEDED "libc++.so" at the shim
(same length, so the change is in place), and checks that every host import then resolves
against the stock image. It also disables the guest libdl's CFI slow path (see disable_guest_cfi).

    python tools/translator.py [--ndk PATH]
"""
import argparse
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'jni'))
from build import find_ndk  # noqa: E402

BUNDLE = os.path.join(ROOT, 'prebuilts', 'digitalis')
SHIM_SOURCE = os.path.join(ROOT, 'native', 'berberis_compat', 'berberis_compat.c')
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
    args = ap.parse_args()

    with open(os.path.join(BUNDLE, 'SHA256SUMS')) as f:
        for line in f:
            digest, rel = line.split()
            with open(os.path.join(BUNDLE, rel), 'rb') as g:
                if hashlib.sha256(g.read()).hexdigest() != digest:
                    sys.exit(f'{rel}: does not match prebuilts/digitalis/SHA256SUMS')
    shutil.rmtree(args.out, ignore_errors=True)
    shutil.copytree(os.path.join(BUNDLE, 'system'), os.path.join(args.out, 'system'))
    disable_guest_cfi(args.out)
    shim = build_shim(find_ndk(args.ndk), os.path.join(args.out, 'system', 'lib64'))
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
    print(f'{len(hosts)} host binaries, {patched} pointed at {SHIM}; all imports resolve -> {args.out}')


if __name__ == '__main__':
    main()
