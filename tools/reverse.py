"""Build a local Horizon implementation reference; all derived files stay under work/.

python tools/reverse.py build
python tools/reverse.py search HeadTracker
python tools/reverse.py search CoreFunctions --limit 30
python tools/reverse.py disassemble /system_ext/lib64/libtrackingserviceclients.so --symbol Head

Java output is faithful smali disassembly, not reconstructed Java source.
Native output is exported symbols, dependencies and strings, not C decompilation.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import struct
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE / 'inventory'))
from inventory import Tree
from dex import Dex
import elf
import smali

APPS = {'com.oculus.vrshell', 'com.oculus.systemdriver',
        'com.oculus.os.cm', 'com.oculus.presence'}
META_CLASS = ('Lcom/oculus/', 'Lcom/meta/', 'Lcom/facebook/', 'Lhorizonos/',
              'Loculus/', 'Landroid/vr/', 'Landroid/hardware/vr/')
NATIVE = re.compile(r'(tracking|memorybroker|runtimeipc|compositor|sensor)', re.I)


def dynamic_symbols(raw):
    """Read ELF64 little-endian SHT_DYNSYM, including undefined imports."""
    if raw[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    offset = struct.unpack_from('<Q', raw, 40)[0]
    size, count = struct.unpack_from('<HH', raw, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', raw, offset + i * size) for i in range(count)]
    tables = [s for s in sections if s[1] == 11]
    if not tables:
        raise ValueError('ELF has no dynamic symbol section')
    for section in tables:
        strings = sections[section[6]]
        table = raw[strings[4]:strings[4] + strings[5]]
        for pos in range(section[4], section[4] + section[5], section[9]):
            name, info, other, index, value, length = struct.unpack_from('<IBBHQQ', raw, pos)
            if not name:
                continue
            symbol = table[name:table.index(b'\0', name)].decode('utf-8', 'replace')
            kind = 'FUNC' if info & 15 == 2 else 'OTHER'
            yield f'{value:016x}\t{length:x}\t{"IMPORT" if index == 0 else "DEFINED"}\t{kind}\t{symbol}'


def key(path):
    return Path(path).name + '-' + hashlib.sha256(path.encode()).hexdigest()[:10]


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding='utf-8', errors='backslashreplace')


def archive_paths(tree, inventory):
    paths = set()
    cps = json.loads((inventory / 'classpaths.json').read_text())
    for jars in cps.values():
        for jar in jars:
            if jar.startswith('/system/framework/') or jar.startswith('/system_ext/framework/'):
                paths.add(jar)
    with (inventory / 'packages.tsv').open(encoding='utf-8') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            if row['package'] in APPS:
                paths.add(row['path'])
    return sorted(paths)


def build(args):
    tree = Tree(str(args.fs.resolve()))
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Separate from compat.py's scratch tree: source jars are never patched.
    db = sqlite3.connect(out / 'index.sqlite')
    db.execute('DROP TABLE IF EXISTS entries')
    db.execute('CREATE TABLE entries(kind TEXT, source TEXT, unit TEXT, value TEXT, artifact TEXT)')
    errors, sources = [], []
    classpath = os.pathsep.join(smali.fetch())
    java = smali.java()

    def add(kind, source, unit, value, artifact):
        # The minimal DEX reader preserves undecodable modified UTF-8 bytes
        # as surrogates; SQLite accepts Unicode scalar values only.
        value = value.encode('utf-8', 'backslashreplace').decode('utf-8')
        db.execute('INSERT INTO entries VALUES (?, ?, ?, ?, ?)',
                   (kind, source, unit, value, str(artifact.relative_to(out))))

    for source in archive_paths(tree, args.inventory):
        path = Path(tree.host(source))
        if not path.is_file():
            errors.append({'source': source, 'error': 'missing extracted file'})
            continue
        print('DEX:', source, flush=True)
        try:
            with zipfile.ZipFile(path) as z:
                units = sorted(n for n in z.namelist() if re.fullmatch(r'classes\d*\.dex', n))
                for unit in units:
                    raw = z.read(unit)
                    d = Dex(raw)
                    classes = set(d.class_names())
                    # Meta classes only in the large AOSP framework jars; the
                    # Meta framework jars and chosen APKs retain all classes.
                    if source.startswith('/system/framework/'):
                        classes = {c for c in classes if c.startswith(META_CLASS)}
                    if not classes:
                        continue
                    base = out / 'java' / key(source) / unit.removesuffix('.dex')
                    dex_path = base / unit
                    write(base / 'classes.txt', '\n'.join(sorted(classes)) + '\n')
                    stamp = base / 'disassembly.sha256'
                    digest = hashlib.sha256(raw).hexdigest()
                    cached = (stamp.is_file() and stamp.read_text() == digest
                              and (base / 'smali').is_dir())
                    dex_path.write_bytes(raw)
                    command = [java, '-Xmx4g', '-cp', classpath, smali.MAINS['baksmali'],
                               'd', str(dex_path), '-o', str(base / 'smali'), '-j', '4']
                    if len(classes) != len(d.class_defs):
                        command += ['--classes', ','.join(sorted(classes))]
                    if not cached:
                        subprocess.run(command, check=True, capture_output=True, text=True)
                        write(stamp, digest)
                    for cls in sorted(classes):
                        artifact = base / 'smali' / (cls[1:-1] + '.smali')
                        add('class', source, unit, cls, artifact)
                    for cls, index, flags, code in d.methods_of_classes():
                        if cls in classes:
                            artifact = base / 'smali' / (cls[1:-1] + '.smali')
                            add('native' if flags & 0x100 else 'method', source, unit,
                                d.method_signature(index), artifact)
                    # All strings are indexed with DEX provenance; some belong
                    # to excluded AOSP classes and are not class-local evidence.
                    for value in d.strings():
                        if len(value) >= 4:
                            add('dex-string', source, unit, value, dex_path)
                    sources.append({'source': source, 'unit': unit,
                                    'sha256': hashlib.sha256(raw).hexdigest(),
                                    'classes': len(classes), 'artifact': str(base.relative_to(out))})
            db.commit()
        except (OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as e:
            errors.append({'source': source, 'error': str(e),
                           'stderr': getattr(e, 'stderr', None)})

    for name in sorted(tree.files('*.so')):
        path = Path(name)
        if 'lib64' not in path.parts or not NATIVE.search(path.name):
            continue
        source = tree.device(str(path))
        if not source:
            continue
        print('ELF:', source, flush=True)
        try:
            info = elf.read(path)
            if not info:
                continue
            base = out / 'native' / key(source)
            write(base / 'dependencies.json', json.dumps(info, indent=2))
            raw = path.read_bytes()
            symbols = list(dynamic_symbols(raw))
            write(base / 'symbols.txt', '\n'.join(symbols) + '\n')
            for line in symbols:
                add('symbol', source, '', line, base / 'symbols.txt')
            strings = []
            for match in re.finditer(rb'[\x20-\x7e]{6,}', raw):
                value = match.group().decode('ascii')
                line = f'0x{match.start():x}\t{value}'
                strings.append(line)
                add('elf-string', source, f'file-offset:0x{match.start():x}', value, base / 'strings.tsv')
            write(base / 'strings.tsv', '\n'.join(strings) + '\n')
            sources.append({'source': source, 'sha256': hashlib.sha256(raw).hexdigest(),
                            'artifact': str(base.relative_to(out)), 'arch': info['arch']})
            db.commit()
        except (OSError, ValueError, subprocess.CalledProcessError) as e:
            errors.append({'source': source, 'error': str(e)})
    count = db.execute('SELECT COUNT(*) FROM entries').fetchone()[0]
    db.close()
    write(out / 'manifest.json', json.dumps({'sources': sources, 'errors': errors}, indent=2))
    write(out / 'README.md', '# Local Horizon reference\n\n'
          'Generated with `python tools/reverse.py build`. Search from the repository root:\n\n'
          '```powershell\npython tools/reverse.py search HeadTracker\n'
          'python tools/reverse.py search CoreFunctions --limit 30\n'
          'python tools/reverse.py search Head --kind symbol --source trackingserviceclients\n'
          'python tools/reverse.py disassemble /system_ext/lib64/libtrackingserviceclients.so --symbol Head\n'
          'rg -n "TRANSACTION_|onTransact|writeInterfaceToken" work/reverse/java\n```\n\n'
          'Java files are smali bytecode disassembly, not original Java source. Native files\n'
          'contain dynamic symbols, ELF dependencies and ASCII strings with file offsets;\n'
          'they are not decompiled C. The optional `disassemble` command uses the Python\n'
          '`capstone` package for exported arm64 functions. C++ symbol names remain mangled.\n'
          'Search strings are leads, not proof of executed behavior.\n'
          'The manifest records original device paths, content hashes and processing errors.\n'
          'Framework AOSP classes are filtered; Meta framework jars and selected APKs are complete.\n'
          'Selected APKs: VrShell, VrDriver, CMSHeadset, PresenceService. Native selection:\n'
          '64-bit libraries whose names contain tracking, memorybroker, runtimeipc, compositor or sensor.\n\n'
          f'Indexed rows: {count}; source units: {len(sources)}; errors: {len(errors)}.\n')
    print(f'{count} entries; {len(sources)} source units; {len(errors)} errors; output: {out}')
    return 1 if errors else 0


def search(args):
    db = sqlite3.connect(f'{(args.out.resolve() / "index.sqlite").as_uri()}?mode=ro', uri=True)
    term = args.term.replace('\\', '\\\\').replace('%', '\\%').replace('_', '\\_')
    rows = db.execute("SELECT kind, source, unit, value, artifact FROM entries "
                      "WHERE value LIKE ? ESCAPE '\\' AND (? IS NULL OR kind = ?) "
                      "AND (? IS NULL OR instr(source, ?) > 0) ORDER BY CASE kind "
                      "WHEN 'class' THEN 0 WHEN 'native' THEN 1 WHEN 'method' THEN 2 "
                      "WHEN 'symbol' THEN 3 ELSE 4 END, source, value LIMIT ?",
                      ('%' + term + '%', args.kind, args.kind, args.source, args.source, args.limit))
    for kind, source, unit, value, artifact in rows:
        print(json.dumps({'kind': kind, 'source': source, 'unit': unit,
                          'value': value, 'artifact': str(args.out / artifact)}, ensure_ascii=True))
    db.close()


def disassemble(args):
    """Disassemble matching exported arm64 functions with their ELF addresses."""
    import capstone
    tree = Tree(str(args.fs.resolve()))
    path = Path(tree.host(args.source))
    raw = path.read_bytes()
    if struct.unpack_from('<H', raw, 18)[0] != 183:
        raise ValueError('native disassembly currently supports arm64 only')
    phoff = struct.unpack_from('<Q', raw, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', raw, 54)
    loads = []
    for i in range(phcount):
        kind, flags, offset, address, _, size = struct.unpack_from('<IIQQQQ', raw, phoff + i * phsize)
        if kind == 1 and flags & 1:
            loads.append((address, offset, size))
    decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    decoder.skipdata = True
    count = 0
    folder = args.out.resolve() / 'native' / key(args.source) / 'assembly'
    for line in dynamic_symbols(raw):
        address, size, state, kind, symbol = line.split('\t', 4)
        if state != 'DEFINED' or kind != 'FUNC' or args.symbol.lower() not in symbol.lower():
            continue
        address, size = int(address, 16), int(size, 16)
        location = next((off + address - va for va, off, length in loads
                         if va <= address and address + size <= va + length), None)
        if location is None or not size:
            continue
        # Hash the symbol to avoid Windows path limits; the header preserves its name.
        artifact = folder / (hashlib.sha256(symbol.encode()).hexdigest()[:16] + '.asm')
        lines = [f'; source: {args.source}', f'; sha256: {hashlib.sha256(raw).hexdigest()}',
                 f'; symbol: {symbol}', f'; ELF address: 0x{address:x}; size: 0x{size:x}',
                 '; Assembly only; branch targets are ELF addresses, not resolved call names.']
        for instruction in decoder.disasm(raw[location:location + size], address):
            lines.append(f'{instruction.address:016x}: {instruction.mnemonic:10} {instruction.op_str}')
        write(artifact, '\n'.join(lines) + '\n')
        print(symbol, '->', artifact)
        count += 1
        if count >= args.limit:
            break
    print(f'Disassembled {count} functions.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build')
    b.add_argument('--fs', type=Path, default=ROOT / 'work/fs')
    b.add_argument('--inventory', type=Path, default=ROOT / 'work/inventory')
    b.add_argument('--out', type=Path, default=ROOT / 'work/reverse')
    s = sub.add_parser('search')
    s.add_argument('term')
    s.add_argument('--limit', type=int, default=20)
    s.add_argument('--kind', choices=['class', 'method', 'native', 'symbol', 'dex-string', 'elf-string'])
    s.add_argument('--source', help='case-sensitive substring of the original device path')
    s.add_argument('--out', type=Path, default=ROOT / 'work/reverse')
    d = sub.add_parser('disassemble')
    d.add_argument('source', help='original device path to an arm64 ELF library')
    d.add_argument('--symbol', required=True, help='substring of the mangled exported symbol')
    d.add_argument('--limit', type=int, default=30)
    d.add_argument('--fs', type=Path, default=ROOT / 'work/fs')
    d.add_argument('--out', type=Path, default=ROOT / 'work/reverse')
    args = parser.parse_args()
    if args.command == 'build':
        return build(args)
    if args.command == 'search':
        return search(args)
    return disassemble(args)


if __name__ == '__main__':
    sys.exit(main())
