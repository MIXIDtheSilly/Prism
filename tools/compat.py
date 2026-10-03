"""Compatibility patches in Horizon's framework jars for stock Android underneath.

* Bridges: stock libandroid_runtime looks some Java members up by exact signature and aborts if
  they're missing. Where Meta added parameters, Prism adds a method with the stock signature that
  calls Meta's version with a default for each new parameter.
* Disabled methods: a few of Meta's methods need Meta's modified ART (stock ART lacks what they
  call). Their bodies are replaced with a plain return.
* Rewrites: other uses of members Meta added to its ART module are replaced wherever they occur
  (a constant for a field, stock's overload, or nothing for a debugging call).

The patched jars are built locally from the user's OTA into work/build/compat/<device path>, which
tools/deploy.py uses instead of the originals.

    python tools/compat.py
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
REGISTER_LOCK_ORDERING = Rewrite(f'{VMRUNTIME}->registerLockOrdering([I)V', drop=True,
                                 reason="lock-order checking through Meta's ART")

# Keyed by device path; each jar is read from the partition image its path names.
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
    ],
    '/system/framework/services.jar': [REGISTER_LOCK_ORDERING],
    '/system_ext/framework/oculus-system-services.jar': [REGISTER_LOCK_ORDERING],
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
                text = disable_method(text, b) if isinstance(b, Disable) else text + '\n' + smali_method(b)
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
    out = io_bytes()
    with zipfile.ZipFile(out, 'w') as dst:
        for info in src.infolist():
            dst.writestr(info, replaced.get(info.filename, src.read(info.filename)), compress_type=info.compress_type)
    return out.getvalue()


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
        if isinstance(b, Disable):
            continue
        for sig in (b.stock, b.meta):
            if f'{b.cls}->{b.name}{sig}' not in have:
                sys.exit(f'verification failed: {b.cls}->{b.name}{sig} missing')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--images', default=os.path.join('work', 'images'))
    ap.add_argument('--out', default=os.path.join('work', 'build', 'compat'))
    args = ap.parse_args()
    images = {}
    for device_path, bridges in BRIDGES.items():
        print(f'{device_path}: {len(bridges)} patches')
        partition = device_path.split('/')[1]
        path = device_path if partition == 'system' else device_path[len(partition) + 1:]
        if partition not in images:
            images[partition] = Ext4(os.path.join(args.images, partition + '.img'))
        data = images[partition].read(images[partition].lookup(path))
        workdir = tempfile.mkdtemp(prefix='prism-compat-')
        try:
            patched = patch_jar(data, bridges, workdir)
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
