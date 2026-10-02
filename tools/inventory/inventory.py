"""Inventory an extracted Horizon OS against stock Android 14 (the emulator's x86_64 image).

Answers the questions Prism's port depends on:
  * classpaths   which jars make up the boot and system_server classpaths
  * jni          native methods Horizon's Java declares that stock libandroid_runtime/servers don't
                 implement, and the reverse (stock natives Horizon dropped), plus Meta JNI libraries
  * packages     every APK: package, shared UID, ABIs, partition, Meta or AOSP
  * daemons      every init service: binary, arch, Meta/vendor libraries, device nodes it names
  * services     binder services seen on a real headset, and which Horizon files name them
  * openxr       XR_* extensions the Meta runtime offers and the system apps ask for

    python tools/inventory/inventory.py [--fs work/fs] [--baseline work/baseline/fs] [--services services.txt]

Writes work/inventory/*.tsv|json and work/inventory/report.md.
"""
import argparse
import collections
import glob
import io
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'ota'))
import axml  # noqa: E402
import dex  # noqa: E402
import elf  # noqa: E402
from protobuf import fields  # noqa: E402

PARTITIONS = ['system_ext', 'product', 'vendor', 'odm', 'vendor_dlkm', 'odm_dlkm']
CLASSPATH_KINDS = {1: 'BOOTCLASSPATH', 2: 'DEX2OATBOOTCLASSPATH', 3: 'SYSTEMSERVERCLASSPATH',
                   4: 'STANDALONE_SYSTEMSERVER_JARS'}
DEFAULT_SERVICES = os.path.expanduser('~/Documents/QuestOnPC/dumps/horizon-dump/services.txt')
XR_EXTENSION = re.compile(rb'XR_[A-Z][A-Z0-9]+_[A-Za-z0-9_]+')
DEV_NODE = re.compile(rb'/dev/[A-Za-z0-9_\-./]{2,64}')


class Tree:
    """Maps device paths (/system/..., /apex/<name>/...) to files in an extracted tree."""

    def __init__(self, root):
        self.root = root
        self.mounts = {'/system': os.path.join(root, 'system', 'system')}
        for p in PARTITIONS:
            if os.path.isdir(os.path.join(root, p)):
                self.mounts['/' + p] = os.path.join(root, p)
        self.apex = os.path.join(root, 'apex')
        self.links, self.paths = {}, set()
        for manifest in glob.glob(os.path.join(root, '**', '*.manifest.tsv'), recursive=True):
            prefix = self._prefix_for(manifest)
            with open(manifest, encoding='utf-8', errors='surrogateescape') as f:
                for line in f:
                    parts = line.rstrip('\n').split('\t')
                    if len(parts) > 1 and not parts[0].startswith('#'):
                        self.paths.add(prefix + parts[0])
                    if len(parts) > 8 and parts[1] == 'l':
                        self.links[prefix + parts[0]] = parts[8]

    def _prefix_for(self, manifest):
        name = os.path.basename(manifest)[:-len('.manifest.tsv')]
        if os.path.basename(os.path.dirname(manifest)) == 'apex':
            return '/apex/' + name
        return '' if name == 'system' else '/' + name

    def host(self, path):
        for _ in range(8):  # follow symlinks recorded in the manifests
            target = self.links.get(path)
            if not target:
                break
            path = target if target.startswith('/') else os.path.normpath(
                os.path.join(os.path.dirname(path), target)).replace('\\', '/')
        if path.startswith('/apex/'):
            name, _, rest = path[6:].partition('/')
            return os.path.join(self.apex, name.split('@')[0], *rest.split('/'))
        for mount in sorted(self.mounts, key=len, reverse=True):
            if path == mount or path.startswith(mount + '/'):
                return os.path.join(self.mounts[mount], *path[len(mount):].split('/'))
        return os.path.join(self.root, 'system', *path.split('/'))

    def device(self, host_path):
        host_path = os.path.normpath(host_path)
        if host_path.startswith(os.path.normpath(self.apex) + os.sep):
            rel = os.path.relpath(host_path, self.apex).replace(os.sep, '/')
            return '/apex/' + rel
        for mount, base in sorted(self.mounts.items(), key=lambda m: len(m[1]), reverse=True):
            base = os.path.normpath(base)
            if host_path.startswith(base + os.sep):
                return mount + '/' + os.path.relpath(host_path, base).replace(os.sep, '/')
        return None

    def files(self, pattern):
        for base in list(self.mounts.values()) + [self.apex]:
            yield from glob.glob(os.path.join(base, '**', pattern), recursive=True)


# ---------------------------------------------------------------- classpaths

def classpaths(tree):
    out = collections.defaultdict(list)
    for pb in sorted(tree.files('*.pb')):
        if os.sep + 'classpaths' + os.sep not in pb:
            continue
        with open(pb, 'rb') as f:
            for jar in fields(f.read()).get(1, []):
                jf = fields(jar)
                kind = CLASSPATH_KINDS.get(jf.get(2, [0])[0], 'UNKNOWN')
                out[kind].append(jf[1][0].decode())
    return dict(out)


# ---------------------------------------------------------------- dex helpers

def jar_dexes(path):
    try:
        with zipfile.ZipFile(path) as z:
            return [dex.Dex(z.read(n)) for n in sorted(z.namelist()) if re.fullmatch(r'classes\d*\.dex', n)]
    except (zipfile.BadZipFile, ValueError, KeyError):
        return []


def natives(dexes):
    return {sig for d in dexes for sig in d.native_methods()}


# ---------------------------------------------------------------- jni

def jni_report(horizon, baseline, cps):
    jars = set(cps.get('BOOTCLASSPATH', []) + cps.get('SYSTEMSERVERCLASSPATH', []) +
               cps.get('STANDALONE_SYSTEMSERVER_JARS', []))
    jars |= {horizon.device(p) for p in horizon.files('*.jar') if os.sep + 'framework' + os.sep in p}
    jars = sorted(j for j in jars if j and not j.startswith('/apex/'))  # mainline modules match stock
    stock_libs = {os.path.basename(p)[3:-3] for p in baseline.paths if p.endswith('.so')}
    lib_names = {os.path.basename(p)[3:-3] for p in horizon.paths if p.endswith('.so')} - stock_libs
    rows = []
    for jar in jars:
        h_dex = jar_dexes(horizon.host(jar))
        if not h_dex:
            continue
        b_path = baseline.host(jar)
        b_dex = jar_dexes(b_path) if os.path.exists(b_path) else []
        h_nat, b_nat = natives(h_dex), natives(b_dex)
        h_strings = {s for d in h_dex for s in d.strings()}
        b_strings = {s for d in b_dex for s in d.strings()}
        libs = sorted(s for s in h_strings - b_strings if s in lib_names)
        rows.append({
            'jar': jar, 'in_stock': bool(b_dex),
            'classes': sum(len(d.class_defs) for d in h_dex),
            'natives': len(h_nat),
            'natives_added': sorted(h_nat - b_nat),
            'natives_removed': sorted(b_nat - h_nat) if b_dex else [],
            'meta_jni_libs': libs,
            'classpath': [k for k, v in cps.items() if jar in v],
        })
    return rows


# ---------------------------------------------------------------- packages

def apk_info(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        root = axml.parse(z.read('AndroidManifest.xml'))
    abis = sorted({n.split('/')[1] for n in names if n.startswith('lib/') and n.count('/') >= 2})
    app = next(root.iter('application'), None)
    return {
        'package': root.get('package', ''),
        'version': str(root.get('versionName', '')),
        'shared_uid': root.get('sharedUserId', ''),
        'abis': ','.join(abis),
        'dex': sum(1 for n in names if re.fullmatch(r'classes\d*\.dex', n)),
        'persistent': bool(app is not None and app.get('persistent')),
        'services': len(list(root.iter('service'))),
        'activities': len(list(root.iter('activity'))),
    }


def packages(horizon, baseline):
    stock = set()
    for p in baseline.files('*.apk'):
        try:
            stock.add(apk_info(p)['package'])
        except Exception:
            pass
    rows = []
    for p in sorted(horizon.files('*.apk')):
        device = horizon.device(p)
        try:
            info = apk_info(p)
        except Exception as e:
            info = {'package': f'?? {e.__class__.__name__}'}
        info['path'] = device
        info['stock'] = info.get('package') in stock
        rows.append(info)
    return rows


# ---------------------------------------------------------------- daemons

def init_services(tree):
    services = []
    for rc in sorted(tree.files('*.rc')):
        device = tree.device(rc) or rc
        with open(rc, encoding='utf-8', errors='replace') as f:
            current = None
            for line in f:
                words = line.split()
                if not words or words[0].startswith('#'):
                    continue
                if words[0] == 'service' and len(words) >= 3:
                    current = {'name': words[1], 'binary': words[2], 'rc': device, 'class': '', 'user': '',
                               'disabled': False, 'interfaces': []}
                    services.append(current)
                elif words[0] in ('on', 'import'):
                    current = None
                elif current is not None:
                    if words[0] == 'class':
                        current['class'] = ' '.join(words[1:])
                    elif words[0] == 'user':
                        current['user'] = words[1]
                    elif words[0] == 'disabled':
                        current['disabled'] = True
                    elif words[0] == 'interface' and len(words) >= 3:
                        current['interfaces'].append(words[2])
    return services


LIB_DIRS = {
    '/system': ['/system/lib64'],
    '/system_ext': ['/system_ext/lib64', '/system/lib64'],
    '/product': ['/product/lib64', '/system/lib64'],
    '/vendor': ['/vendor/lib64', '/odm/lib64', '/apex/com.android.vndk.v34/lib64'],
    '/odm': ['/odm/lib64', '/vendor/lib64', '/apex/com.android.vndk.v34/lib64'],
}
APEX_LIB_DIRS = ['/apex/com.android.runtime/lib64/bionic', '/apex/com.android.art/lib64',
                 '/apex/com.android.i18n/lib64', '/apex/com.android.os.statsd/lib64',
                 '/apex/com.android.conscrypt/lib64', '/apex/com.meta.hzos/lib64']


def library_closure(tree, binary, stock_libs):
    """Resolves DT_NEEDED transitively. Returns (arch, {lib: device path or None})."""
    info = elf.read(tree.host(binary)) if os.path.isfile(tree.host(binary)) else None
    if not info:
        return None, {}
    part = '/' + binary.split('/')[1]
    if binary.startswith('/apex/'):
        part = '/system'
        search = ['/'.join(binary.split('/')[:3]) + '/lib64']
    else:
        search = []
    search += LIB_DIRS.get(part, ['/system/lib64']) + APEX_LIB_DIRS
    if part != '/system':
        search += ['/system/lib64']
    resolved = {}
    queue = list(info['needed'])
    while queue:
        name = queue.pop()
        if name in resolved:
            continue
        resolved[name] = None
        for d in search:
            host = tree.host(f'{d}/{name}')
            if os.path.isfile(host):
                resolved[name] = f'{d}/{name}'
                sub = elf.read(host)
                if sub:
                    queue.extend(sub['needed'])
                break
    return info['arch'], resolved


def daemons(horizon, baseline):
    stock_libs = {os.path.basename(p) for p in baseline.files('*.so')}
    stock_bins = {p for p in baseline.paths if '/bin/' in p}
    rows = []
    for svc in init_services(horizon):
        binary = svc['binary']
        arch, libs = library_closure(horizon, binary, stock_libs)
        meta_libs = sorted(n for n, p in libs.items() if p and n not in stock_libs and not p.startswith('/vendor')
                           and not p.startswith('/odm') and '/com.android.vndk' not in p)
        vendor_libs = sorted(n for n, p in libs.items() if p and (p.startswith('/vendor') or p.startswith('/odm')))
        missing = sorted(n for n, p in libs.items() if p is None)
        nodes = set()
        for path in [binary] + [p for n, p in libs.items() if p and n in meta_libs]:
            host = horizon.host(path)
            if os.path.isfile(host):
                with open(host, 'rb') as f:
                    nodes.update(m.decode() for m in DEV_NODE.findall(f.read()))
        rows.append(dict(svc, arch=arch or 'missing', stock=binary in stock_bins,
                         meta_libs=meta_libs, vendor_libs=vendor_libs, missing_libs=missing,
                         dev_nodes=sorted(nodes)))
    return rows


# ---------------------------------------------------------------- binder services

def binder_services(horizon, services_txt, pkg_rows, jni_rows):
    entries = []
    with open(services_txt, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\d+\s+(\S+):\s*\[(.*)\]', line.strip())
            if m:
                entries.append((m.group(1), m.group(2)))
    # Candidate files that could register a service: Meta daemons and libraries, Meta jars, Meta APKs' dex.
    blobs = []
    for p in horizon.files('*'):
        dev = horizon.device(p) or ''
        if not os.path.isfile(p):
            continue
        if re.search(r'/(bin|lib64)/', dev) and (dev.startswith('/system_ext') or dev.startswith('/apex/com.meta')
                                                  or dev.startswith('/odm') or dev.startswith('/product')):
            blobs.append((dev, p, None))
    for row in jni_rows:
        if not row['in_stock'] or row['jar'].endswith(('/framework.jar', '/services.jar')):
            blobs.append((row['jar'], horizon.host(row['jar']), 'zip'))
    for row in pkg_rows:
        if not row.get('stock') and row.get('dex'):
            blobs.append((row['path'], horizon.host(row['path']), 'zip'))
    names = {n.encode() for n, _ in entries} | {i.encode() for _, i in entries if i}
    pattern = re.compile(b'|'.join(re.escape(n) for n in sorted(names, key=len, reverse=True)))
    hits = collections.defaultdict(set)
    for dev, host, kind in blobs:
        datas = []
        if kind == 'zip':
            try:
                with zipfile.ZipFile(host) as z:
                    datas = [z.read(n) for n in z.namelist() if n.endswith('.dex')]
            except zipfile.BadZipFile:
                continue
        else:
            with open(host, 'rb') as f:
                datas = [f.read()]
        for data in datas:
            for m in set(pattern.findall(data)):
                hits[m.decode()].add(dev)
    rows = []
    for name, iface in entries:
        aosp = re.match(r'(android\.|com\.android\.|android$)', iface or name) is not None
        files = sorted(hits.get(name, set()) | hits.get(iface, set()))
        rows.append({'name': name, 'interface': iface, 'aosp': aosp, 'named_in': files})
    return rows


# ---------------------------------------------------------------- openxr

def openxr(horizon, pkg_rows):
    rows = []
    for row in pkg_rows:
        if row.get('stock') or not row.get('abis'):
            continue
        exts = set()
        with zipfile.ZipFile(horizon.host(row['path'])) as z:
            for n in z.namelist():
                if n.startswith('lib/arm64-v8a/') and n.endswith('.so'):
                    exts.update(m.decode() for m in XR_EXTENSION.findall(z.read(n)))
        exts = {e for e in exts if not e.endswith('_SPEC_VERSION') and not e.endswith('_EXTENSION_NAME')
                and re.fullmatch(r'XR_[A-Z0-9]+_[a-z0-9_]+', e)}
        if exts:
            rows.append({'package': row['package'], 'path': row['path'], 'extensions': sorted(exts)})
    return rows


# ---------------------------------------------------------------- output

def write_tsv(path, rows, columns):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(columns) + '\n')
        for r in rows:
            f.write('\t'.join(','.join(v) if isinstance(v := r.get(c, ''), list) else str(v) for c in columns) + '\n')


def report(out, cps, jni, pkgs, dmns, svcs, xr):
    lines = ['# Horizon OS inventory', '', 'Generated by `tools/inventory/inventory.py`. Stock = the Android '
             'Emulator API 34 x86_64 image.', '']
    lines += ['## Classpaths', '']
    for kind, jars in cps.items():
        meta = [j for j in jars if not j.startswith('/apex/com.android') and not j.startswith('/system/framework/')]
        lines.append(f'- **{kind}**: {len(jars)} jars; non-stock locations: {", ".join(meta) or "none"}')
    lines += ['', '## JNI', '', '| jar | stock | natives | added vs stock | removed vs stock | Meta JNI libs |',
              '|---|---|---|---|---|---|']
    for r in jni:
        if r['natives'] or r['natives_removed'] or not r['in_stock']:
            lines.append(f'| {r["jar"]} | {"yes" if r["in_stock"] else "**no**"} | {r["natives"]} | '
                         f'{len(r["natives_added"])} | {len(r["natives_removed"])} | {", ".join(r["meta_jni_libs"])} |')
    meta_pkgs = [p for p in pkgs if not p.get('stock')]
    lines += ['', '## Packages', '', f'{len(pkgs)} APKs, {len(meta_pkgs)} not in stock Android. '
              f'{sum(p.get("shared_uid") == "android.uid.system" for p in meta_pkgs)} of those run as the system UID; '
              f'{sum("arm64-v8a" in p.get("abis", "") for p in meta_pkgs)} carry arm64 native code; '
              f'{sum(not p.get("abis") for p in meta_pkgs)} are Java only.', '']
    meta_d = [d for d in dmns if not d['stock']]
    lines += ['## Native daemons', '', f'{len(dmns)} init services, {len(meta_d)} with binaries not in stock Android.',
              '', '| service | binary | vendor libs | device nodes |', '|---|---|---|---|']
    for d in meta_d:
        if d['binary'].startswith(('/system_ext', '/apex/com.meta', '/odm', '/product')) or d['meta_libs']:
            lines.append(f'| {d["name"]} | {d["binary"]} | {len(d["vendor_libs"])} | '
                         f'{", ".join(d["dev_nodes"][:6])}{" …" if len(d["dev_nodes"]) > 6 else ""} |')
    non_aosp = [s for s in svcs if not s['aosp']]
    lines += ['', '## Binder services', '', f'{len(svcs)} services on the reference headset, {len(non_aosp)} non-AOSP; '
              f'{sum(not s["named_in"] for s in non_aosp)} of those are not named in any file of this build.', '']
    lines += ['## OpenXR extensions', '']
    for r in xr:
        lines.append(f'- **{r["package"]}**: {len(r["extensions"])}')
    with open(os.path.join(out, 'report.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fs', default=os.path.join('work', 'fs'))
    ap.add_argument('--baseline', default=os.path.join('work', 'baseline', 'fs'))
    ap.add_argument('--services', default=DEFAULT_SERVICES, help='`service list` output from a headset')
    ap.add_argument('-o', '--out', default=os.path.join('work', 'inventory'))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    horizon, baseline = Tree(args.fs), Tree(args.baseline)

    def step(name):
        print(f'{name}...', flush=True)

    step('classpaths')
    cps = classpaths(horizon)
    step('jni')
    jni = jni_report(horizon, baseline, cps)
    step('packages')
    pkgs = packages(horizon, baseline)
    step('daemons')
    dmns = daemons(horizon, baseline)
    svcs = []
    if os.path.exists(args.services):
        step('binder services')
        svcs = binder_services(horizon, args.services, pkgs, jni)
    step('openxr')
    xr = openxr(horizon, pkgs)

    def dump(name, data):
        with open(os.path.join(args.out, name), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, indent=1)
    dump('classpaths.json', cps)
    dump('jni.json', jni)
    dump('daemons.json', dmns)
    dump('services.json', svcs)
    dump('openxr.json', xr)
    write_tsv(os.path.join(args.out, 'packages.tsv'), pkgs,
              ['package', 'path', 'stock', 'shared_uid', 'abis', 'dex', 'persistent', 'services', 'activities',
               'version'])
    report(args.out, cps, jni, pkgs, dmns, svcs, xr)
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
