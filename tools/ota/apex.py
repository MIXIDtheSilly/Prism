"""Unpack every APEX module found in the extracted partitions.

Each .apex is a zip holding apex_manifest.pb and apex_payload.img, a filesystem image that
Android mounts at /apex/<name>. The payload goes to work/images/apex/<name>.img and its files to
work/fs/apex/<name>, with a manifest like the partitions get.

    python tools/ota/apex.py [--fs work/fs] [--images work/images]
"""
import argparse
import glob
import os
import shutil
import struct
import sys
import zipfile

import ext4
from protobuf import fields


def apex_name(archive):
    manifest = archive.read('apex_manifest.pb')
    f = fields(manifest)
    return f[1][0].decode(), f.get(2, [0])[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fs', default=os.path.join('work', 'fs'))
    ap.add_argument('--images', default=os.path.join('work', 'images'))
    args = ap.parse_args()

    image_dir = os.path.join(args.images, 'apex')
    fs_dir = os.path.join(args.fs, 'apex')
    os.makedirs(image_dir, exist_ok=True)
    found = sorted(p for p in glob.glob(os.path.join(args.fs, '*', '**', '*.apex'), recursive=True)
                   if not os.path.relpath(p, args.fs).startswith('apex'))
    if not found:
        sys.exit(f'no .apex files under {args.fs}; extract the partitions first')
    for path in found:
        with zipfile.ZipFile(path) as archive:
            name, version = apex_name(archive)
            image = os.path.join(image_dir, name + '.img')
            with archive.open('apex_payload.img') as src, open(image, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 24)
        with open(image, 'rb') as f:
            head = f.read(2048)
        if struct.unpack_from('<H', head, 1080)[0] != 0xEF53:
            print(f'{name}: payload is not ext4, kept as {image} only')
            continue
        out = os.path.join(fs_dir, name)
        rows = ext4.extract(ext4.Ext4(image), out)
        ext4.write_manifest(out, rows)
        print(f'{name} v{version} ({os.path.relpath(path, args.fs)}): {sum(r[1] == "f" for r in rows)} files')


if __name__ == '__main__':
    main()
