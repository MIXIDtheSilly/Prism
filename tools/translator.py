"""Prepare the Digitalis ARM64 translator (prebuilts/digitalis) for Prism's Android 14 base.

Digitalis is built for Android 16. Its host binaries import a few symbols Android 14's x86_64
libraries lack (see native/berberis_compat). This builds that shim (libpcx.so), copies the
bundle to work/build/translator/, points each host binary's DT_NEEDED "libc++.so" at the shim
(same length, so the change is in place), and checks that every host import then resolves
against the stock image. It also disables the guest libdl's CFI slow path (see disable_guest_cfi)
and keeps block chaining's per-thread state usable through thread exit (see keep_chain_sites),
and builds two proxies of Prism's: the PC's libm for the guest's (build_proxy_libm) and VrShell's
ray hit-testing lifted to native code (build_proxy_libshell; needs capstone, else skipped).

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
import zipfile

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
    return [BUNDLE, SHIM_SOURCE, PROXY_LIBM_SOURCE, PROXY_LIBSHELL_SOURCE, os.path.join(HERE, 'lift_arm64.py'), VRSHELL_APK,
            __file__, bionic, *extracted(os.path.join(ROOT, 'work', 'baseline', 'fs'))]
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

    def vaddr(self, offset):
        return next(va + offset - off for va, off, size in self.loads if off <= offset < off + size)

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


# The bundle's translator and its proxy libraries (rebuilt by Refract, prebuilts/digitalis/MANIFEST.txt)
# check the first argument of their proxies' marshallers: from 1 to 0xffff one writes "BAD proxy
# PRE-EXTRACT" and the guest's registers to stderr and twice to the log, then makes the call anyway.
# A zero-length memcpy or memcmp from an empty container passes such a pointer, and crcs does ~40 a
# second, the most of any log source. Prism makes each check's jump past the logging unconditional
# (ja rel32 -> jmp rel32; nop), where the logging it skips is the message's.
PROXY_CHECK = bytes.fromhex('483dfeff00000f87')  # cmp rax, 0xfffe; ja rel32
PROXY_CHECK_MESSAGE = b'BAD proxy PRE-EXTRACT'


def quiet_proxy_checks(out_dir):
    total = 0
    for path in sorted(glob.glob(os.path.join(out_dir, 'system', 'lib64', 'libberberis_*.so'))):
        if os.path.basename(path) == PROXY_LIBM:
            continue
        with open(path, 'rb') as f:
            elf = Elf(f.read())
        data = elf.data
        message = data.find(PROXY_CHECK_MESSAGE + b':')
        if message == -1:
            continue
        message = elf.vaddr(message)
        sites, at = [], data.find(PROXY_CHECK)
        while at != -1:
            ja = at + 6
            rel, = struct.unpack_from('<i', data, ja + 2)
            # The message's address, loaded rip-relative (lea r, [rip + d]) on the path the jump skips.
            skipped = range(ja + 6, ja + 6 + rel - 7)
            if not any(data[i] in (0x48, 0x4C) and data[i + 1] == 0x8D and data[i + 2] & 0xC7 == 0x05 and
                       elf.vaddr(i) + 7 + struct.unpack_from('<i', data, i + 3)[0] == message for i in skipped):
                sys.exit(f'{os.path.basename(path)}: a check at {at:#x} that does not skip the message')
            sites.append(ja)
            at = data.find(PROXY_CHECK, at + 1)
        if not sites:
            sys.exit(f'{os.path.basename(path)}: "{PROXY_CHECK_MESSAGE.decode()}" but no check found')
        for ja in sites:
            rel, = struct.unpack_from('<i', data, ja + 2)
            data[ja:ja + 6] = b'\xe9' + struct.pack('<i', rel + 1) + b'\x90'
        with open(path, 'wb') as f:
            f.write(data)
        total += len(sites)
    return total


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


# VrShell's hottest code as native code (native/berberis_proxy_libshell): tools/lift_arm64.py turns
# these libshell.so functions into C, from the user's own VrShell (work/fs), and the proxy's runtime
# checks the build and the bytes again in VrShell before libprism_jni has the translator use them.
# The offsets are this Horizon build's; another one is skipped here (and would be at runtime). A
# size of None: the function is the code reachable from its address.
PROXY_LIBSHELL_SOURCE = os.path.join(ROOT, 'native', 'berberis_proxy_libshell', 'proxy_libshell.c')
PROXY_LIBSHELL = 'libberberis_proxy_libshell.so'
VRSHELL_APK = os.path.join(ROOT, 'work', 'fs', 'system_ext', 'priv-app', 'VrShell', 'VrShell.apk')
LIBSHELL = 'lib/arm64-v8a/libshell.so'
LIBSHELL_BUILD_ID = '4b820048d9ddd5c6be6ed20598244dd5cc4efb6f'
LIBSHELL_LIFTED = {  # address: (size, what)
    0x9cb994: (1332, 'HitTestableHelpers::ObjectRayTrace'),
    0x9cb700: (660, 'a 4x4 matrix inverse'),
    0x9cbec8: (388, 'TransformHelpers::GetUnitYAxis'),
    0x9cc04c: (456, 'TransformHelpers::GetNormal'),
    0x9ce650: (1068, "HitTestable: a shape's model parameters (GetModelParams)"),
    0xb14a8c: (128, 'a rotation from an angle (sincosf), for GetModelParams'),
    0xb0fe8c: (332, 'transform math, for GetModelParams'),
    0xb15bb4: (16, "a shape type's name, for the asserts' messages"),
    # Each frame's transforms and animation.
    0xdc85bc: (None, "a hand's 26 joint poses, from its tracked pose"),
    0x957dbc: (None, "a ClayEntity's world transform (its parent's first, when dirty)"),
    0x9c5078: (None, 'HsrPlayerInputSystem: pose math'),
    0x9c54f4: (None, 'HsrPlayerInputSystem: pose math'),
    0x9c57c0: (None, 'HsrPlayerInputSystem: pose math'),
    0xb42d3c: (None, 'transform math'),
    0xe63f6c: (None, "VrPlatformOpenXr: the tracking space's location math"),
    0xc0dfc4: (None, 'HzAnimPlayback: sampling animations'),
    0xc103b4: (None, 'HzAnim: pose math'),
    0xe5c6b0: (None, 'AnimationPlayback::fillBoneDataImpl (its entry setting the flag)'),
    0xe5c6b8: (None, 'AnimationPlayback::fillBoneDataImpl'),
    0xbcf42c: (None, 'transform composition'),
    0xe1b820: (None, "the renderer's bounds math"),
    0x95e0c8: (None, 'a tree walk, for the hand grip ray'),
}
# And the rest of its hottest code, the 178 functions next in a frame-thread profile (2026-10-27):
# call-heavy C++, about 6000 calls out to translated code, which pays now that such a call is
# cheap (native/berberis_proxy_libshell/proxy_libshell.c: prism_guest_call). With the Store, the
# library, Browser and TV open, VrShell's frame thread went from 10.1 ms a frame to 8.6 (9.3 with
# only the functions above and the cheap calls).
LIBSHELL_HOT = (
    0xa6a860, 0x92b95c, 0x93aee4, 0x958b34, 0x9584fc, 0xb0b024, 0x989f44, 0x9e57f8, 0xea1ff4, 0x926dac,
    0x92c054, 0x92c7b4, 0x92a160, 0x94cea4, 0xa16a48, 0x9eb130, 0x9cce28, 0x9582a4, 0x93bda8, 0xb42940,
    0xb0a580, 0x9e394c, 0xa98e50, 0x91ed0c, 0x95b2c4, 0x92f38c, 0xb91bf8, 0x9266b0, 0xdf9b50, 0x932e70,
    0x9ca278, 0x9512c4, 0xa2d54c, 0xa99bb0, 0xed3024, 0xeb459c, 0x973a9c, 0xea2ea4, 0x9d4acc, 0xa5ca28,
    0x13375fc, 0xbcc668, 0xe64404, 0x92c4b0, 0x9c1cd4, 0x962b30, 0x91e82c, 0xb2dbb8, 0x9e5180, 0x92ae58,
    0xad6b20, 0xa8dd90, 0x987ba4, 0x9cdeb4, 0xe1b5e8, 0x988368, 0x9214c8, 0x99def4, 0x929cd8, 0x94b1c0,
    0x94fd58, 0xc0c330, 0x9cb314, 0x93f300, 0xa0b8f0, 0x94a370, 0x177a6c8, 0xb9f464, 0x93565c, 0x980230,
    0xe820d8, 0x9201d4, 0xddccd4, 0x931208, 0x95c2ec, 0xe76ca8, 0x25418b4, 0x9cf7e0, 0xbb82a4, 0x91ea34,
    0x9eaa4c, 0x92ab38, 0x1338db4, 0xe1a5c8, 0x9bebe4, 0x989ef4, 0x949d68, 0x927468, 0x958a10, 0xa904cc,
    0xe87040, 0x9a3e70, 0x9edcec, 0xa6ff88, 0x928ddc, 0x95351c, 0xa98764, 0x9bb93c, 0xc35e78, 0x926374,
    0xcb87d0, 0xb0c208, 0x925ec0, 0x9c1384, 0xd067a8, 0x952804, 0xa2de84, 0x9d1594, 0xa17894, 0x9ac338,
    0x94dafc, 0x9554b8, 0x9ec8ac, 0x9c6480, 0x935994, 0x9e45f4, 0xa1f138, 0x93371c, 0x980a48, 0x9b953c,
    0x93e8f0, 0x9c06d4, 0xad1a80, 0xa06e34, 0xb31f9c, 0xa72730, 0xe8e93c, 0xbcd1f0, 0xb9ed80, 0x954338,
    0xa588f0, 0x93f9f4, 0x97045c, 0xa5032c, 0xa0b270, 0xcb45e0, 0x92e974, 0x9ac458, 0xe9e00c, 0x9624d4,
    0x988d2c, 0x9ef840, 0xddc814, 0xacef1c, 0xafcfe4, 0x955290, 0x939aa8, 0x9caeb8, 0xbb7aa0, 0xa820c0,
    0x965edc, 0xe1babc, 0x987100, 0xddd600, 0x9f1608, 0xd05c4c, 0xb71f8c, 0x9c88b0, 0x929178, 0x9669d0,
    0x97d3e0, 0x9b7a64, 0xa2637c, 0xcadf70, 0xe1bcd4, 0x96dc28, 0xcbcc40, 0xb1a368, 0x9cead0, 0xec2588,
    0xcaf710, 0x9bd760, 0x9d80b0, 0xa9b1e8, 0x1852084, 0x92788c, 0x9d0e34, 0x9cee7c,
)


def libshell_functions():
    """{address: (size, what)} of everything lifted from libshell.so."""
    return {**LIBSHELL_LIFTED, **{a: (None, 'hot') for a in LIBSHELL_HOT if a not in LIBSHELL_LIFTED}}


LIBSHELL_EXTERNS = {'sincosf': 'sincosf', 'atan2f': 'atan2f', 'abort': 'abort', '__stack_chk_fail': 'stack_chk_fail',
                    0xa6a440: 'assert', 'memcpy': 'memcpy', 'memmove': 'memmove', 'memset': 'memset',
                    **{name: name for name in ('sinf', 'cosf', 'tanf', 'asinf', 'acosf', 'atanf', 'expf', 'exp2f',
                                               'logf', 'log2f', 'log10f', 'powf', 'fmodf')}}


def build_proxy_libshell(ndk, lib_dir):
    """The lifted hit-testing, or why not (another Horizon build, no capstone)."""
    try:
        import lift_arm64
    except ImportError as e:
        return f'not built ({e.name} is not installed; VrShell hit-tests translated)'
    if not os.path.exists(VRSHELL_APK):
        return f'not built (no {os.path.relpath(VRSHELL_APK, ROOT)})'
    with zipfile.ZipFile(VRSHELL_APK) as apk:
        data = apk.read(LIBSHELL)
    build_id = lift_arm64.Elf(data).build_id()
    if not build_id or build_id.hex() != LIBSHELL_BUILD_ID:
        return f'not built (VrShell\'s libshell.so is build {build_id.hex() if build_id else "?"}, not {LIBSHELL_BUILD_ID})'
    functions = libshell_functions()
    source, _lifter = lift_arm64.lift_library(data, functions, LIBSHELL_EXTERNS,
                                              f'VrShell.apk:{LIBSHELL}')
    work = tempfile.mkdtemp(prefix='prism-lift-')
    try:
        lifted = os.path.join(work, 'lifted_libshell.c')
        with open(lifted, 'w', newline='\n') as f:
            f.write(source)
        host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
        clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
        # No -mfma: only the lifted functions are built for FMA (their own pragma), as the runtime
        # checks the CPU has it before any of them runs.
        subprocess.run([clang, '--target=x86_64-linux-android34', '-shared', '-fPIC', '-O2', '-Wall',
                        '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-label',
                        '-Wno-misleading-indentation', PROXY_LIBSHELL_SOURCE, lifted,
                        '-o', os.path.join(lib_dir, PROXY_LIBSHELL), f'-Wl,-soname,{PROXY_LIBSHELL}', f'-L{lib_dir}',
                        '-Wl,--no-as-needed', '-lberberis_arm64', '-llog', '-lm', '-Wl,--allow-shlib-undefined',
                        '-Wl,--build-id=sha1'], check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return f'{len(functions)} VrShell functions lifted'


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
    quieted = quiet_proxy_checks(args.out)
    ndk = find_ndk(args.ndk)
    shim = build_shim(ndk, os.path.join(args.out, 'system', 'lib64'))
    build_proxy_libm(ndk, os.path.join(args.out, 'system', 'lib64'))
    intercepted = intercept_more_libm(args.out, proxy_names())
    lifted = build_proxy_libshell(ndk, os.path.join(args.out, 'system', 'lib64'))
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
          f'{quieted} proxy checks quieted; {lifted}; '
          f'all imports resolve -> {args.out}')


if __name__ == '__main__':
    main()
