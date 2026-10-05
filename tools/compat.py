"""Compatibility patches in Horizon's framework jars (and an app) for stock Android underneath.

* Bridges: stock libandroid_runtime looks some Java members up by exact signature and aborts if
  they're missing. Where Meta added parameters, Prism adds a method with the stock signature that
  calls Meta's version with a default for each new parameter.
* Disabled methods: a few of Meta's methods need Meta's modified ART (stock ART lacks what they
  call). Their bodies are replaced with a plain return.
* Rewrites: other uses of members Meta added to its ART module are replaced wherever they occur
  (a constant for a field, stock's overload, or nothing for a debugging call).
* Replaced methods: where Horizon on an x86_64 host needs different behaviour, a method's body is
  replaced with Prism's (smali).

The patched jars are built locally from the user's OTA into work/build/compat/<device path>, which
tools/deploy.py uses instead of the originals.

    python tools/compat.py [device path ...]
"""
import argparse
import os
import re
import shutil
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'ota'))
sys.path.insert(0, os.path.join(HERE, 'inventory'))
import dex  # noqa: E402
import smali  # noqa: E402
from ext4 import Ext4  # noqa: E402


class Bridge:
    """A method with stock's signature that forwards to Meta's, inserting defaults.

    inserts maps a parameter index in Meta's signature to the value passed there: None (null or
    zero) or a string constant."""

    def __init__(self, cls, name, stock, meta, inserts, invoke='direct', reason=''):
        self.cls, self.name, self.stock, self.meta = cls, name, stock, meta
        self.inserts, self.invoke, self.reason = inserts, invoke, reason


class Disable:
    """Replaces a void method's body with return-void."""

    def __init__(self, cls, method, reason=''):
        self.cls, self.method, self.reason = cls, method, reason


class Replace:
    """Replaces a method's body with smali (from .registers through the last instruction)."""

    def __init__(self, cls, method, body, reason=''):
        self.cls, self.method, self.body, self.reason = cls, method, body, reason


class Splice:
    """Replaces the one occurrence of old (smali lines) in a method with new."""

    def __init__(self, cls, method, old, new, reason=''):
        self.cls, self.method, self.old, self.new, self.reason = cls, method, old, new, reason


class Add:
    """Adds a method (complete smali, .method through .end method) to a class."""

    def __init__(self, cls, smali, reason=''):
        self.cls, self.smali, self.reason = cls, smali, reason


class Rewrite:
    """Replaces each use of ref, a member stock's modules lack, in every class of the jar.

    constant: a static int field read becomes this value
    call:     an invocation calls this stock method instead, with the leading arguments it takes
    drop:     an invocation of a void method is removed"""

    def __init__(self, ref, constant=None, call=None, drop=False, reason=''):
        self.ref, self.constant, self.call, self.drop, self.reason = ref, constant, call, drop, reason

    def apply(self, text):
        if self.constant is not None:
            pattern = re.compile(rf'^(\s*)sget (v\d+|p\d+), {re.escape(self.ref)}$', re.M)
            return pattern.sub(lambda m: f'{m.group(1)}const/16 {m.group(2)}, {self.constant:#x}', text)
        pattern = re.compile(rf'^(\s*)(invoke-\w+)(/range)? \{{([^}}]*)\}}, {re.escape(self.ref)}$', re.M)
        return pattern.sub(self._invoke, text)

    def _invoke(self, m):
        indent, op, ranged, regs = m.groups()
        if self.drop:
            return f'{indent}nop'
        words = (0 if op == 'invoke-static' else 1) + sum(map(width, params(self.call[self.call.index('('):])))
        if ranged:
            first = regs.split('..')[0].strip()
            last = f'{first[0]}{int(first[1:]) + words - 1}'
            return f'{indent}{op}/range {{{first} .. {last}}}, {self.call}'
        kept = [r.strip() for r in regs.split(',')][:words]
        return f'{indent}{op} {{{", ".join(kept)}}}, {self.call}'


VMRUNTIME = 'Ldalvik/system/VMRuntime;'
GUEST_DIR = '/system/lib64/arm64/prism'  # Horizon's arm64 libraries; see tools/deploy.py
SYSTEM_LIBRARY_PATH = f'''
.method static prismSystemLibraryPath(Landroid/content/pm/ApplicationInfo;Ljava/lang/String;)Ljava/lang/String;
    .registers 4
    iget-object v0, p0, Landroid/content/pm/ApplicationInfo;->primaryCpuAbi:Ljava/lang/String;
    if-eqz v0, :host
    invoke-static {{v0}}, {VMRUNTIME}->getInstructionSet(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    invoke-static {{}}, {VMRUNTIME}->getCurrentInstructionSet()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {{v0, v1}}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z
    move-result v0
    if-nez v0, :host
    const-string v0, "{GUEST_DIR}"
    return-object v0
    :host
    return-object p1
.end method
'''
PM = 'Lcom/android/server/pm'
ABIS = f'{PM}/PackageAbiHelper$Abis;'
PACKAGE = f'{PM}/pkg/AndroidPackage;'
# Android looks for a bundled app's libraries only in lib/<ISA of the first 64-bit ABI>: x86_64 on
# Prism, where Meta's apps have lib/arm64. The stock check runs first; failing it, the other 64-bit
# ABIs are tried in order, so arm64 apps get arm64-v8a and run their libraries through the bridge.
BUNDLED_APP_ABIS = f'''    .registers 10
    invoke-interface {{p1}}, {PACKAGE}->getPath()Ljava/lang/String;
    move-result-object v0
    invoke-static {{v0}}, {PM}/PackageAbiHelperImpl;->deriveCodePathName(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    invoke-interface {{p1}}, {PACKAGE}->getBaseApkPath()Ljava/lang/String;
    move-result-object v1
    invoke-static {{v1}}, {PM}/PackageAbiHelperImpl;->calculateBundledApkRoot(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v1
    invoke-direct {{p0, p1, v1, v0}}, {PM}/PackageAbiHelperImpl;->getBundledAppAbi({PACKAGE}Ljava/lang/String;Ljava/lang/String;){ABIS}
    move-result-object v0
    iget-object v1, v0, {ABIS}->primary:Ljava/lang/String;
    if-nez v1, :done
    new-instance v1, Ljava/io/File;
    invoke-interface {{p1}}, {PACKAGE}->getPath()Ljava/lang/String;
    move-result-object v2
    invoke-direct {{v1, v2}}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-static {{v1}}, Landroid/content/pm/parsing/ApkLiteParseUtils;->isApkFile(Ljava/io/File;)Z
    move-result v2
    if-nez v2, :done
    new-instance v2, Ljava/io/File;
    const-string v3, "lib"
    invoke-direct {{v2, v1, v3}}, Ljava/io/File;-><init>(Ljava/io/File;Ljava/lang/String;)V
    sget-object v3, Landroid/os/Build;->SUPPORTED_64_BIT_ABIS:[Ljava/lang/String;
    array-length v4, v3
    const/4 v5, 0x1
    :next
    if-ge v5, v4, :done
    aget-object v6, v3, v5
    invoke-static {{v6}}, {VMRUNTIME}->getInstructionSet(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v7
    new-instance v8, Ljava/io/File;
    invoke-direct {{v8, v2, v7}}, Ljava/io/File;-><init>(Ljava/io/File;Ljava/lang/String;)V
    invoke-virtual {{v8}}, Ljava/io/File;->exists()Z
    move-result v7
    if-eqz v7, :skip
    new-instance v0, {ABIS}
    const/4 v7, 0x0
    invoke-direct {{v0, v6, v7}}, {ABIS}-><init>(Ljava/lang/String;Ljava/lang/String;)V
    goto :done
    :skip
    add-int/lit8 v5, v5, 0x1
    goto :next
    :done
    return-object v0
'''
REGISTER_LOCK_ORDERING = Rewrite(f'{VMRUNTIME}->registerLockOrdering([I)V', drop=True,
                                 reason="lock-order checking through Meta's ART")

# OCMS, Horizon's content service, publishes the library (LibrarySynced: the Universal Menu's tiles,
# VrShell's AppManagerClient) once the Store channel's content is resolved. A fetch that finds
# content resolves it; one that finds none (EMPTY: no Meta account here) skips that, so the library
# never got ready, OCMS refetched every 15 s and the tiles stayed loading. An empty fetch is resolved
# like one that found content: an empty Store library, with the apps installed on the device.
OCMS_FETCHED = 'Lcom/oculus/ocms/am/init/AppManagerInternal$onLibraryContentFetched$1;'
FETCH_STATUS = 'Lcom/oculus/ocms/am/common/ChannelFetchStatus;'
RESOLVE_EMPTY_FETCH = Splice(
    OCMS_FETCHED, 'run()V',
    f'sget-object v0, {FETCH_STATUS}->SUCCESS:{FETCH_STATUS}',
    f'''    sget-object v0, {FETCH_STATUS}->EMPTY:{FETCH_STATUS}
    if-ne v1, v0, :prism_not_empty
    sget-object v1, {FETCH_STATUS}->SUCCESS:{FETCH_STATUS}
    :prism_not_empty
    sget-object v0, {FETCH_STATUS}->SUCCESS:{FETCH_STATUS}''',
    reason='an empty fetch is resolved too (no Meta account: the library has only local apps)')

# Meta's SurfaceFlinger gives each window's input handle an XrWindowInfo (its volumetric window, pose,
# size and shape), which the volumetric window service follows. Stock SurfaceFlinger has none to give,
# and the listener threw on the first window, once for every change of windows. Windows without one
# are skipped: the service keeps the placements VrShell gives it.
VW_LISTENER = 'Loculus/internal/volumetricwindow/VwWindowInfosListener;'
VW_CHANGED = 'onWindowInfosChanged([Landroid/view/InputWindowHandle;[Landroid/window/WindowInfosListener$DisplayInfo;)V'
SKIP_WINDOWS_WITHOUT_XR_INFO = [
    Splice(VW_LISTENER, VW_CHANGED, 'aget-object v3, p1, v2',
           '''    aget-object v3, p1, v2
    iget-object v4, v3, Landroid/view/InputWindowHandle;->xrWindowInfo:Landroid/view/XrWindowInfo;
    if-eqz v4, :prism_next''',
           reason="windows without Meta's XrWindowInfo (stock SurfaceFlinger's)"),
    Splice(VW_LISTENER, VW_CHANGED, 'add-int/lit8 v2, v2, 0x1',
           '''    :prism_next
    add-int/lit8 v2, v2, 0x1''',
           reason="windows without Meta's XrWindowInfo (stock SurfaceFlinger's)"),
]

# Keyed by device path; each jar (or APK) is read from the partition image its path names.
BRIDGES = {
    '/system/framework/framework.jar': [
        Bridge('Landroid/view/InputDevice;', '<init>',
               '(IIILjava/lang/String;IILjava/lang/String;ZIILandroid/view/KeyCharacterMap;Ljava/lang/String;'
               'Ljava/lang/String;ZZZZZIII)V',
               '(IIILjava/lang/String;IILjava/lang/String;Ljava/lang/String;ZIILandroid/view/KeyCharacterMap;'
               'Ljava/lang/String;Ljava/lang/String;ZZZZZIII)V',
               {6: ''}, reason='Meta added mUniqueId before mDescriptor'),
        Bridge('Landroid/app/backup/FullBackupDataOutput;', '<init>', '(JI)V', '(JII)V', {2: None},
               reason='Meta added mMaxFilesToLog (0 = no logging)'),
        Bridge('Landroid/app/backup/FullBackupDataOutput;', 'addSize', '(J)V', '(JLjava/lang/String;)V', {1: None},
               invoke='virtual', reason='Meta added the file name, only used for logging'),
        Disable('Lcom/android/internal/os/Lockdep;', 'registerHandler(Landroid/content/Context;)V',
                reason="lock-order checking through Meta's ART (VMRuntime.setLockOrderingViolationLogger)"),
        Rewrite('Landroid/system/OsConstants;->RLIMIT_RTPRIO:I', constant=14,
                reason="Meta's libcore adds the constant; 14 on Linux"),
        Rewrite(f'{VMRUNTIME}->clampGrowthLimit(J)V', call=f'{VMRUNTIME}->clampGrowthLimit()V',
                reason="Meta's ART takes the limit; stock's clamps to the configured one"),
        # A bundled app's library path ends with java.library.path (/system/lib64:/system_ext/lib64).
        # For an app running translated, those are host libraries, and the guest linker stops at the
        # first one of the wrong architecture; its system libraries are the guest's (GUEST_DIR).
        Add('Landroid/app/LoadedApk;', SYSTEM_LIBRARY_PATH, reason="translated apps' system libraries"),
        Splice('Landroid/app/LoadedApk;',
               'makePaths(Landroid/app/ActivityThread;ZLandroid/content/pm/ApplicationInfo;Ljava/util/List;Ljava/util/List;)V',
               '''const-string p1, "java.library.path"
                  invoke-static {p1}, Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;
                  move-result-object p1''',
               '''    const-string p1, "java.library.path"
    invoke-static {p1}, Ljava/lang/System;->getProperty(Ljava/lang/String;)Ljava/lang/String;
    move-result-object p1
    invoke-static {p2, p1}, Landroid/app/LoadedApk;->prismSystemLibraryPath(Landroid/content/pm/ApplicationInfo;Ljava/lang/String;)Ljava/lang/String;
    move-result-object p1''',
               reason="translated apps' system libraries"),
    ],
    '/system/framework/services.jar': [
        REGISTER_LOCK_ORDERING,
        Replace(f'{PM}/PackageAbiHelperImpl;', f'getBundledAppAbis({PACKAGE}){ABIS}', BUNDLED_APP_ABIS,
                reason="bundled apps' libraries for a translated ABI (Meta's apps have lib/arm64)"),
    ],
    '/system_ext/framework/oculus-system-services.jar': [REGISTER_LOCK_ORDERING, *SKIP_WINDOWS_WITHOUT_XR_INFO],
    '/system_ext/priv-app/OCMS/OCMS.apk': [RESOLVE_EMPTY_FETCH],
}


def params(signature):
    inside = signature[1:signature.index(')')]
    return re.findall(r'\[*(?:L[^;]+;|[ZBSCIJFD])', inside)


def width(t):
    return 2 if t in ('J', 'D') else 1


def move(t):
    if t.startswith(('L', '[')):
        return 'move-object/from16'
    return 'move-wide/from16' if width(t) == 2 else 'move/from16'


def smali_method(b):
    stock, meta = params(b.stock), params(b.meta)
    static = b.invoke == 'static'
    locals_ = sum(map(width, meta)) + (0 if static else 1)
    words = sum(map(width, stock)) + (0 if static else 1)
    kind = 'constructor ' if b.name == '<init>' else ''
    lines = [f'.method public {kind}blacklist {b.name}{b.stock}',
             f'    .registers {locals_ + words}',
             f'    # Prism bridge: {b.reason}']
    dest, src = 0, 0
    if not static:
        lines.append('    move-object/from16 v0, p0')
        dest, src = 1, 1
    stock_iter = iter(stock)
    for i, t in enumerate(meta):
        if i in b.inserts:
            value = b.inserts[i]
            if value is None:
                lines.append(f'    const-wide/16 v{dest}, 0x0' if width(t) == 2 else f'    const/4 v{dest}, 0x0')
            else:
                lines.append(f'    const-string v{dest}, "{value}"')
        else:
            st = next(stock_iter)
            if st != t:
                raise ValueError(f'{b.cls}{b.name}: stock parameter {st} lines up with Meta parameter {t}')
            lines.append(f'    {move(t)} v{dest}, p{src}')
            src += width(t)
        dest += width(t)
    ret = b.meta[b.meta.index(')') + 1:]
    if ret != 'V':
        raise ValueError('only void bridges are supported')
    lines += [f'    invoke-{b.invoke}/range {{v0 .. v{dest - 1}}}, {b.cls}->{b.name}{b.meta}', '    return-void',
              '.end method', '']
    return '\n'.join(lines)


def disable_method(text, d):
    pattern = re.compile(r'(\.method [^\n]*?' + re.escape(d.method) + r'\n)(.*?)(\.end method)', re.S)
    if not pattern.search(text):
        raise ValueError(f'{d.cls}->{d.method} not found')
    if not d.method.endswith(')V'):
        raise ValueError('only void methods can be disabled')
    return pattern.sub(lambda m: f'{m.group(1)}    .registers {registers_for(d.method)}\n'
                                 f'    # Prism: disabled, {d.reason}\n    return-void\n{m.group(3)}', text, count=1)


def splice_method(text, s):
    pattern = re.compile(r'(\.method [^\n]*?' + re.escape(s.method) + r'\n)(.*?)(\.end method)', re.S)
    m = pattern.search(text)
    if not m:
        raise ValueError(f'{s.cls}->{s.method} not found')
    # baksmali puts a .line directive between instructions; match the old lines with any in between.
    lines = [re.escape(line.strip()) for line in s.old.strip().splitlines()]
    old = re.compile(r'^\s*' + r'\n(?:\s*\.line \d+\n|\s*\n)*\s*'.join(lines) + r'$', re.M)
    found = old.findall(m.group(2))
    if len(found) != 1:
        raise ValueError(f'{s.cls}->{s.method}: expected one match of the spliced code, found {len(found)}')
    body = old.sub(lambda _: f'    # Prism: {s.reason}\n{s.new.rstrip()}', m.group(2), count=1)
    return text[:m.start(2)] + body + text[m.end(2):]


def replace_method(text, r):
    pattern = re.compile(r'(\.method [^\n]*?' + re.escape(r.method) + r'\n)(.*?)(\.end method)', re.S)
    if not pattern.search(text):
        raise ValueError(f'{r.cls}->{r.method} not found')
    return pattern.sub(lambda m: f'{m.group(1)}    # Prism: replaced, {r.reason}\n{r.body}{m.group(3)}', text, count=1)


def registers_for(method):
    # Parameter registers plus "this"; enough for any instance or static method with no locals.
    return sum(map(width, params(method[method.index('('):]))) + 1


def patch_jar(data, bridges, workdir):
    """Returns the jar bytes with the patches applied to the dex files that define their classes."""
    by_class = {}
    for b in bridges:
        if not isinstance(b, Rewrite):
            by_class.setdefault(b.cls, []).append(b)
    rewrites = [b for b in bridges if isinstance(b, Rewrite)]
    unused = {r.ref for r in rewrites}
    src = zipfile.ZipFile(io_bytes(data))
    replaced = {}
    for name in sorted(src.namelist()):
        if not re.fullmatch(r'classes\d*\.dex', name):
            continue
        raw = src.read(name)
        parsed = dex.Dex(raw)
        defined = set(parsed.class_names()) & set(by_class)
        used = [r for r in rewrites if r.ref in parsed.references()]
        if not defined and not used:
            continue
        dex_path = os.path.join(workdir, name)
        out_dir = os.path.join(workdir, name + '.smali')
        with open(dex_path, 'wb') as f:
            f.write(raw)
        smali.run('baksmali', 'd', dex_path, '-o', out_dir)
        patched = set()
        for cls in defined:
            path = os.path.join(out_dir, *cls[1:-1].split('/')) + '.smali'
            with open(path, encoding='utf-8') as f:
                text = f.read()
            for b in by_class.pop(cls):
                if isinstance(b, Disable):
                    text = disable_method(text, b)
                elif isinstance(b, Replace):
                    text = replace_method(text, b)
                elif isinstance(b, Splice):
                    text = splice_method(text, b)
                elif isinstance(b, Add):
                    text += f'\n# Prism: {b.reason}\n{b.smali.strip()}\n'
                else:
                    text += '\n' + smali_method(b)
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            patched.add(cls[1:-1])
        for base, _dirs, files in os.walk(out_dir):
            for file in files:
                path = os.path.join(base, file)
                with open(path, encoding='utf-8') as f:
                    text = f.read()
                new = text
                for r in used:
                    if r.ref in new:
                        new = r.apply(new)
                        unused.discard(r.ref)
                if new != text:
                    with open(path, 'w', encoding='utf-8', newline='\n') as f:
                        f.write(new)
                    patched.add(os.path.relpath(path, out_dir)[:-len('.smali')].replace(os.sep, '/'))
        new_dex = os.path.join(workdir, name + '.new')
        smali.run('smali', 'a', out_dir, '-o', new_dex, '--api', '34')
        with open(new_dex, 'rb') as f:
            replaced[name] = f.read()
        print(f'  {name}: patched {", ".join(sorted(patched))}')
    if by_class:
        sys.exit(f'classes not found in the jar: {", ".join(by_class)}')
    if unused:
        sys.exit(f'members to rewrite that the jar never uses: {", ".join(sorted(unused))}')
    return replaced


def repack_jar(data, replaced):
    src = zipfile.ZipFile(io_bytes(data))
    out = io_bytes()
    with zipfile.ZipFile(out, 'w') as dst:
        for info in src.infolist():
            dst.writestr(info, replaced.get(info.filename, src.read(info.filename)), compress_type=info.compress_type)
    return out.getvalue()


def repack_apk(data, replaced):
    """The APK with some entries' contents replaced, as PackageManager takes it from a system
    partition: entries keep their order and compression, stored ones stay aligned (4 bytes, native
    libraries 4 KiB, as zipalign leaves them), and the APK Signing Block stays before the central
    directory. On system partitions PackageManager reads the signers' certificates without
    verifying the contents, so the APK keeps its signers."""
    import struct
    import zlib
    eocd = data.rindex(b'PK\x05\x06')
    count, cd_size, cd_offset = struct.unpack_from('<HII', data, eocd + 10)
    block = b''
    if data[cd_offset - 16:cd_offset] == b'APK Sig Block 42':
        size = struct.unpack_from('<Q', data, cd_offset - 24)[0]
        block = data[cd_offset - size - 8:cd_offset]
    entries, at = [], cd_offset
    for _ in range(count):
        fields = list(struct.unpack_from('<4s6H3I5H2I', data, at))
        name_len, extra_len, comment_len = fields[10:13]
        entries.append((fields, data[at + 46:at + 46 + name_len + extra_len + comment_len]))
        at += 46 + name_len + extra_len + comment_len
    body, central = io_bytes(), io_bytes()
    for fields, tail in sorted(entries, key=lambda e: e[0][16]):
        flags, method, crc, csize, usize, name_len = fields[3], fields[4], fields[7], fields[8], fields[9], fields[10]
        name = tail[:name_len]
        local = fields[16]
        local_name, local_extra = struct.unpack_from('<2H', data, local + 26)
        start = local + 30 + local_name + local_extra
        payload = data[start:start + csize]
        content = replaced.get(name.decode())
        if content is not None:
            crc, usize = zlib.crc32(content), len(content)
            if method == 0:
                payload = content
            else:
                packer = zlib.compressobj(9, zlib.DEFLATED, -15)
                payload = packer.compress(content) + packer.flush()
            csize = len(payload)
        flags &= ~0x8  # sizes in the headers, no data descriptor
        offset = body.tell()
        pad = 0
        if method == 0:
            align = 4096 if name.endswith(b'.so') else 4
            pad = -(offset + 30 + name_len) % align
        body.write(struct.pack('<4s5H3I2H', b'PK\x03\x04', fields[2], flags, method, fields[5], fields[6],
                               crc, csize, usize, name_len, pad) + name + b'\0' * pad + payload)
        fields[3], fields[7], fields[8], fields[9], fields[16] = flags, crc, csize, usize, offset
        central.write(struct.pack('<4s6H3I5H2I', *fields) + tail)
    head = body.getvalue() + block
    return head + central.getvalue() + struct.pack('<4s4H2IH', b'PK\x05\x06', 0, 0, count, count,
                                                   len(central.getvalue()), len(head), 0)


def io_bytes(data=None):
    import io
    return io.BytesIO(data) if data is not None else io.BytesIO()


def verify(data, bridges):
    z = zipfile.ZipFile(io_bytes(data))
    have, refs = set(), set()
    for n in z.namelist():
        if re.fullmatch(r'classes\d*\.dex', n):
            d = dex.Dex(z.read(n))
            have |= {d.method_signature(i) for _c, i, _f, _code in d.methods_of_classes()}
            refs |= d.references()
    for b in bridges:
        if isinstance(b, Rewrite):
            if b.ref in refs:
                sys.exit(f'verification failed: {b.ref} is still used')
            continue
        if isinstance(b, (Disable, Replace, Splice)):
            continue
        if isinstance(b, Add):
            name = re.search(r'^\.method [^\n]*?(\S+)$', b.smali.strip(), re.M).group(1)
            if f'{b.cls}->{name}' not in have:
                sys.exit(f'verification failed: {b.cls}->{name} missing')
            continue
        for sig in (b.stock, b.meta):
            if f'{b.cls}->{b.name}{sig}' not in have:
                sys.exit(f'verification failed: {b.cls}->{b.name}{sig} missing')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--images', default=os.path.join('work', 'images'))
    ap.add_argument('--out', default=os.path.join('work', 'build', 'compat'))
    ap.add_argument('only', nargs='*', help='device paths to patch (default: all)')
    args = ap.parse_args()
    unknown = set(args.only) - set(BRIDGES)
    if unknown:
        sys.exit(f'nothing to patch in {", ".join(sorted(unknown))}')
    images = {}
    for device_path, bridges in BRIDGES.items():
        if args.only and device_path not in args.only:
            continue
        print(f'{device_path}: {len(bridges)} patches')
        partition = device_path.split('/')[1]
        path = device_path if partition == 'system' else device_path[len(partition) + 1:]
        if partition not in images:
            images[partition] = Ext4(os.path.join(args.images, partition + '.img'))
        data = images[partition].read(images[partition].lookup(path))
        workdir = tempfile.mkdtemp(prefix='prism-compat-')
        try:
            replaced = patch_jar(data, bridges, workdir)
            patched = (repack_apk if device_path.endswith('.apk') else repack_jar)(data, replaced)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)
        verify(patched, bridges)
        out = os.path.join(args.out, *device_path.strip('/').split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(patched)
        print(f'  -> {out}')


if __name__ == '__main__':
    main()
