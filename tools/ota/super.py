"""Unpack the logical partitions of an Android dynamic "super" partition (like AOSP's lpunpack).

Accepts a raw super image, or a GPT disk image (such as the Android Emulator's system.img) that
contains a partition named "super".

    python tools/ota/super.py SDK/system-images/android-34/google_apis/x86_64/system.img -o work/baseline/images
"""
import argparse
import os
import struct
import sys

GEOMETRY_MAGIC, HEADER_MAGIC = 0x616C4467, 0x414C5030
SECTOR = 512


def find_super(f):
    """Returns the byte offset of the super partition (0 for a raw super image)."""
    f.seek(SECTOR)
    gpt = f.read(92)
    if gpt[:8] != b'EFI PART':
        return 0
    entries_lba, count, size = struct.unpack_from('<QII', gpt, 72)
    f.seek(entries_lba * SECTOR)
    table = f.read(count * size)
    for i in range(count):
        entry = table[i * size:(i + 1) * size]
        if entry[56:128].decode('utf-16le').rstrip('\0') == 'super':
            return struct.unpack_from('<Q', entry, 32)[0] * SECTOR
    sys.exit('GPT disk has no partition named "super"')


def read_metadata(f, base):
    f.seek(base + 4096)
    geometry = f.read(52)
    magic, _size = struct.unpack_from('<II', geometry)
    if magic != GEOMETRY_MAGIC:
        sys.exit('no LP metadata geometry found')
    _max_size, _slots, _block = struct.unpack_from('<III', geometry, 40)
    f.seek(base + 4096 * 3)  # reserved 4K, primary + backup geometry, then slot 0 metadata
    header = f.read(256)
    magic, _major, _minor, header_size = struct.unpack_from('<IHHI', header)
    if magic != HEADER_MAGIC:
        sys.exit('no LP metadata header found')
    tables_size = struct.unpack_from('<I', header, 44)[0]
    descriptors = [struct.unpack_from('<III', header, 80 + 12 * i) for i in range(4)]
    f.seek(base + 4096 * 3 + header_size)
    tables = f.read(tables_size)
    partitions_d, extents_d = descriptors[0], descriptors[1]
    extents = [struct.unpack_from('<QIQI', tables, extents_d[0] + i * extents_d[2]) for i in range(extents_d[1])]
    out = []
    for i in range(partitions_d[1]):
        raw = tables[partitions_d[0] + i * partitions_d[2]:][:partitions_d[2]]
        name = raw[:36].rstrip(b'\0').decode()
        _attrs, first, count, _group = struct.unpack_from('<IIII', raw, 36)
        out.append((name, extents[first:first + count]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('-o', '--out', default=os.path.join('work', 'baseline', 'images'))
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    with open(args.image, 'rb') as f:
        base = find_super(f)
        partitions = read_metadata(f, base)
        if args.list:
            for name, extents in partitions:
                print(f'{name:20} {sum(e[0] for e in extents) * SECTOR / 2**20:9.1f} MiB')
            return
        os.makedirs(args.out, exist_ok=True)
        for name, extents in partitions:
            if not extents:
                continue
            path = os.path.join(args.out, name + '.img')
            with open(path, 'wb') as out:
                for sectors, target_type, target, _device in extents:
                    if target_type == 1:  # zero extent
                        out.seek(sectors * SECTOR, 1)
                        continue
                    f.seek(base + target * SECTOR)
                    remaining = sectors * SECTOR
                    while remaining:
                        chunk = f.read(min(remaining, 1 << 24))
                        out.write(chunk)
                        remaining -= len(chunk)
                out.truncate()
            print(f'{name} -> {path}')


if __name__ == '__main__':
    main()
