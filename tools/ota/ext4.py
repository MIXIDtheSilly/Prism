"""Read-only ext4 reader: extract an Android partition image on Windows, without mounting it.

Windows can't hold Linux ownership, modes, SELinux labels, capabilities or symlinks, so they are
written to <out>.manifest.tsv beside the extracted tree. Prism's image builder reads that manifest
back; the extracted files are for inspection and analysis.

    python tools/ota/ext4.py work/images/system.img [-o work/fs/system]
    python tools/ota/ext4.py work/images/system.img --ls /system/framework
"""
import argparse
import mmap
import os
import struct
import sys

S_IFMT, S_IFDIR, S_IFREG, S_IFLNK = 0o170000, 0o040000, 0o100000, 0o120000
EXTENTS_FL, INLINE_DATA_FL = 0x80000, 0x10000000
XATTR_PREFIXES = {1: 'user.', 2: 'system.posix_acl_access', 3: 'system.posix_acl_default', 4: 'trusted.',
                  6: 'security.', 7: 'system.', 8: 'system.richacl'}
TYPE_LETTER = {S_IFDIR: 'd', S_IFREG: 'f', S_IFLNK: 'l'}


class Inode:
    __slots__ = ('number', 'mode', 'uid', 'gid', 'size', 'flags', 'block', 'xattrs')


class Ext4:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        sb = self.map[1024:2048]
        if struct.unpack_from('<H', sb, 56)[0] != 0xEF53:
            sys.exit(f'{path}: not an ext4 image')
        self.block_size = 1024 << struct.unpack_from('<I', sb, 24)[0]
        self.inodes_per_group = struct.unpack_from('<I', sb, 40)[0]
        self.inode_size = struct.unpack_from('<H', sb, 88)[0]
        incompat = struct.unpack_from('<I', sb, 96)[0]
        self.desc_size = struct.unpack_from('<H', sb, 254)[0] if incompat & 0x80 else 32
        first_data_block = struct.unpack_from('<I', sb, 20)[0]
        self.desc_start = (first_data_block + 1) * self.block_size
        self.volume_name = sb[120:136].rstrip(b'\0').decode(errors='replace')

    def inode(self, number):
        group, index = divmod(number - 1, self.inodes_per_group)
        desc = self.desc_start + group * self.desc_size
        table = struct.unpack_from('<I', self.map, desc + 8)[0]
        if self.desc_size >= 64:
            table |= struct.unpack_from('<I', self.map, desc + 0x28)[0] << 32
        off = table * self.block_size + index * self.inode_size
        raw = self.map[off:off + self.inode_size]
        ino = Inode()
        ino.number = number
        ino.mode = struct.unpack_from('<H', raw, 0)[0]
        ino.uid = struct.unpack_from('<H', raw, 2)[0] | struct.unpack_from('<H', raw, 120)[0] << 16
        ino.gid = struct.unpack_from('<H', raw, 24)[0] | struct.unpack_from('<H', raw, 122)[0] << 16
        ino.size = struct.unpack_from('<I', raw, 4)[0] | struct.unpack_from('<I', raw, 108)[0] << 32
        ino.flags = struct.unpack_from('<I', raw, 32)[0]
        ino.block = raw[40:100]
        ino.xattrs = {}
        if self.inode_size > 128:
            extra = struct.unpack_from('<H', raw, 128)[0]
            start = 128 + extra
            if start + 4 <= len(raw) and struct.unpack_from('<I', raw, start)[0] == 0xEA020000:
                self._xattrs(raw, start + 4, start + 4, ino.xattrs)
        acl = struct.unpack_from('<I', raw, 104)[0] | struct.unpack_from('<H', raw, 118)[0] << 32
        if acl:
            block = self.map[acl * self.block_size:(acl + 1) * self.block_size]
            self._xattrs(block, 32, 0, ino.xattrs)
        return ino

    @staticmethod
    def _xattrs(buf, pos, value_base, out):
        while pos + 16 <= len(buf) and struct.unpack_from('<I', buf, pos)[0] != 0:
            name_len, index, value_off, _inum, value_size = struct.unpack_from('<BBHII', buf, pos)
            name = XATTR_PREFIXES.get(index, f'{index}.') + buf[pos + 16:pos + 16 + name_len].decode(errors='replace')
            out[name] = bytes(buf[value_base + value_off:value_base + value_off + value_size])
            pos += (16 + name_len + 3) & ~3

    def extents(self, ino):
        """Yields (logical block, physical block, count, initialized) for a file's data."""
        if not ino.flags & EXTENTS_FL:
            yield from self._block_map(ino)
            return
        yield from self._extent_node(ino.block)

    def _extent_node(self, node):
        magic, entries, _max, depth = struct.unpack_from('<HHHH', node, 0)
        if magic != 0xF30A:
            raise ValueError('bad extent header')
        for i in range(entries):
            off = 12 + i * 12
            if depth == 0:
                logical, length, hi, lo = struct.unpack_from('<IHHI', node, off)
                yield logical, hi << 32 | lo, length & 0x7FFF if length > 0x8000 else length, length <= 0x8000
            else:
                _logical, lo, hi = struct.unpack_from('<IIH', node, off)
                child = (hi << 32 | lo) * self.block_size
                yield from self._extent_node(self.map[child:child + self.block_size])

    def _block_map(self, ino):
        # Legacy ext2/3 indirect blocks: only direct ones are expected in Android images.
        pointers = struct.unpack_from('<15I', ino.block)
        for i, block in enumerate(pointers[:12]):
            if block:
                yield i, block, 1, True
        if any(pointers[12:]):
            raise ValueError(f'inode {ino.number}: indirect block maps are not supported')

    def read(self, ino):
        if ino.flags & INLINE_DATA_FL or (ino.mode & S_IFMT == S_IFLNK and ino.size < 60):
            return bytes(ino.block[:ino.size])  # inline data / fast symlink
        data = bytearray(ino.size)
        bs = self.block_size
        for logical, physical, count, initialized in self.extents(ino):
            if not initialized:
                continue
            start = logical * bs
            if start >= ino.size:
                continue
            length = min(count * bs, ino.size - start)
            data[start:start + length] = self.map[physical * bs:physical * bs + length]
        return bytes(data)

    def listdir(self, ino):
        data = self.read(ino)
        pos = 0
        while pos + 8 <= len(data):
            number, rec_len, name_len = struct.unpack_from('<IHB', data, pos)
            if rec_len < 8:
                break
            if number:
                name = data[pos + 8:pos + 8 + name_len].decode('utf-8', 'surrogateescape')
                if name not in ('.', '..'):
                    yield name, number
            pos += rec_len

    def walk(self, ino=None, path=''):
        """Yields (path, inode) for every entry, depth first, starting with the root."""
        ino = ino or self.inode(2)
        yield path or '/', ino
        for name, number in sorted(self.listdir(ino)):
            child = self.inode(number)
            child_path = f'{path}/{name}'
            if child.mode & S_IFMT == S_IFDIR:
                yield from self.walk(child, child_path)
            else:
                yield child_path, child

    def lookup(self, path):
        ino = self.inode(2)
        for part in filter(None, path.split('/')):
            found = dict(self.listdir(ino)).get(part)
            if found is None:
                raise FileNotFoundError(path)
            ino = self.inode(found)
        return ino


def capability_text(raw):
    if not raw or len(raw) < 12:
        return ''
    _magic, permitted_lo, _inh_lo = struct.unpack_from('<III', raw)
    permitted_hi = struct.unpack_from('<I', raw, 12)[0] if len(raw) >= 20 else 0
    return f'0x{permitted_hi << 32 | permitted_lo:x}'


_WINDOWS_BAD = set('<>:"\\|?*')


def windows_name(name):
    return ''.join('_' if c in _WINDOWS_BAD or ord(c) < 32 else c for c in name)


def extract(fs, out_dir):
    """Writes the tree under out_dir and returns the manifest rows."""
    rows = []
    used = {}  # case-folded Windows path -> image path, to catch names NTFS would merge
    for path, ino in fs.walk():
        kind = ino.mode & S_IFMT
        letter = TYPE_LETTER.get(kind, 'o')
        target = fs.read(ino).decode('utf-8', 'surrogateescape') if kind == S_IFLNK else ''
        rel = '/'.join(windows_name(p) for p in path.split('/') if p)
        key = rel.casefold()
        if key in used and used[key] != path:
            rel += f'~{ino.number}'  # collision on a case-insensitive disk; the manifest keeps the real name
        used[key] = path
        host = os.path.join(out_dir, *rel.split('/')) if rel else out_dir
        if kind == S_IFDIR:
            os.makedirs(host, exist_ok=True)
        elif kind == S_IFREG:
            with open(host, 'wb') as f:
                f.write(fs.read(ino))
        rows.append((path, letter, f'{ino.mode & 0o7777:04o}', ino.uid, ino.gid,
                     ino.xattrs.get('security.selinux', b'').rstrip(b'\0').decode(),
                     capability_text(ino.xattrs.get('security.capability')),
                     ino.size if kind == S_IFREG else '', target, rel if rel != path.lstrip('/') else ''))
    return rows


def write_manifest(out_dir, rows):
    with open(out_dir.rstrip('/\\') + '.manifest.tsv', 'w', encoding='utf-8', errors='surrogateescape',
              newline='\n') as f:
        f.write('# path\ttype\tmode\tuid\tgid\tselinux\tcapabilities\tsize\tlink_target\thost_name\n')
        for row in rows:
            f.write('\t'.join(map(str, row)) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('-o', '--out', help='output directory (default: work/fs/<image name>)')
    ap.add_argument('--ls', metavar='PATH', help='list one directory and exit')
    args = ap.parse_args()
    fs = Ext4(args.image)
    if args.ls:
        for name, number in sorted(fs.listdir(fs.lookup(args.ls))):
            ino = fs.inode(number)
            print(f'{TYPE_LETTER.get(ino.mode & S_IFMT, "o")} {ino.mode & 0o7777:04o} {ino.uid:5} {ino.gid:5} '
                  f'{ino.size:10}  {name}')
        return
    out = args.out or os.path.join('work', 'fs', os.path.splitext(os.path.basename(args.image))[0])
    os.makedirs(out, exist_ok=True)
    rows = extract(fs, out)
    write_manifest(out, rows)
    print(f'{args.image}: {sum(r[1] == "f" for r in rows)} files, {sum(r[1] == "d" for r in rows)} dirs, '
          f'{sum(r[1] == "l" for r in rows)} symlinks -> {out}')


if __name__ == '__main__':
    main()
