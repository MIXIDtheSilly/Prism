"""Members of Horizon's Java mainline modules that stock's lack, and which of Horizon's jars and
apps use them.

Prism keeps stock's mainline modules (APEXes) underneath Horizon's framework. Horizon's are
newer, and Meta's ART adds members of its own; a use of a member stock's module lacks fails at
run time (NoSuchFieldError, NoSuchMethodError) unless tools/compat.py rewrites it. This lists
them all at once.

Stock's module jars come from the running emulator (pulled to work/device/javalib).

    python tools/inventory/module_api.py
"""
import argparse
import glob
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dex  # noqa: E402
import emulator  # noqa: E402

def dexes(path):
    z = zipfile.ZipFile(path)
    for name in z.namelist():
        if re.fullmatch(r'classes\d*\.dex', name):
            yield dex.Dex(z.read(name))


def defined(jars):
    out = set()
    for jar in jars:
        for d in dexes(jar):
            out |= d.defined_members()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fs', default=os.path.join('work', 'fs'), help="Horizon's extracted partitions")
    ap.add_argument('--stock', default=os.path.join('work', 'device', 'javalib'))
    args = ap.parse_args()
    modules = sorted(os.path.basename(os.path.dirname(p))
                     for p in glob.glob(os.path.join(args.fs, 'apex', '*', 'javalib')))
    for module in modules:
        out = os.path.join(args.stock, module)
        if not glob.glob(os.path.join(out, '*.jar')):
            os.makedirs(out, exist_ok=True)
            emulator.adb('pull', f'/apex/{module}/javalib/.', out, check=False)
    horizon = defined(j for m in modules for j in glob.glob(os.path.join(args.fs, 'apex', m, 'javalib', '*.jar')))
    stock = defined(j for m in modules for j in glob.glob(os.path.join(args.stock, m, '*.jar')))
    meta_only = horizon - stock
    print(f'{len(meta_only)} members only in Horizon\'s modules; used ones:')

    users = {}
    paths = []
    for partition in ('system/system', 'system_ext', 'product'):
        root = os.path.join(args.fs, *partition.split('/'))
        paths += glob.glob(os.path.join(root, 'framework', '*.jar'))
        paths += glob.glob(os.path.join(root, 'priv-app', '*', '*.apk'))
        paths += glob.glob(os.path.join(root, 'app', '*', '*.apk'))
    for path in paths:
        try:
            for d in dexes(path):
                for ref in d.references() & meta_only:
                    users.setdefault(ref, set()).add(os.path.relpath(path, args.fs).replace(os.sep, '/'))
        except (zipfile.BadZipFile, ValueError) as e:
            print(f'  (skipped {path}: {e})')
    for ref in sorted(users):
        print(f'  {ref}\n      <- {", ".join(sorted(users[ref]))}')


if __name__ == '__main__':
    main()
