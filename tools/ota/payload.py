"""Extract partition images from an Android A/B OTA (payload.bin, or the OTA zip that holds it).

Only full OTAs are supported: every operation must build its blocks from payload data alone
(REPLACE, REPLACE_BZ, REPLACE_XZ, ZSTD, ZERO, DISCARD). Delta OTAs need the previous images.

    python tools/ota/payload.py OTA.zip|payload.bin [-o work/images] [-p system,vendor] [--list]
"""
import argparse
import bz2
import concurrent.futures
import hashlib
import io
import lzma
import os
import struct
import sys
import zipfile

from protobuf import fields, first

# Partitions that make up the Android userspace. The rest of a Quest OTA is Qualcomm firmware
# (xbl, tz, adsp, ...) that Prism never runs.
ANDROID_PARTITIONS = ['system', 'system_ext', 'product', 'vendor', 'odm', 'vendor_dlkm', 'odm_dlkm',
                      'boot', 'vendor_boot', 'dtbo', 'vbmeta', 'vbmeta_system']

OP_NAMES = {0: 'REPLACE', 1: 'REPLACE_BZ', 2: 'MOVE', 3: 'BSDIFF', 4: 'SOURCE_COPY', 5: 'SOURCE_BSDIFF',
            6: 'ZERO', 7: 'DISCARD', 8: 'REPLACE_XZ', 9: 'PUFFDIFF', 10: 'BROTLI_BSDIFF', 11: 'ZUCCHINI',
            12: 'LZ4DIFF_BSDIFF', 13: 'LZ4DIFF_PUFFDIFF', 14: 'ZSTD'}


def open_payload(path):
    """Returns a seekable binary stream positioned at the start of payload.bin."""
    if zipfile.is_zipfile(path):
        archive = zipfile.ZipFile(path)
        info = archive.getinfo('payload.bin')
        if info.compress_type != zipfile.ZIP_STORED:
            sys.exit('payload.bin is compressed inside the zip; unzip it first')
        stream = open(path, 'rb')
        stream.seek(info.header_offset)
        header = stream.read(30)
        name_len, extra_len = struct.unpack('<HH', header[26:30])
        return _Window(stream, info.header_offset + 30 + name_len + extra_len, info.file_size)
    return _Window(open(path, 'rb'), 0, os.path.getsize(path))


class _Window(io.RawIOBase):
    """A read-only view of [start, start+size) in another file."""

    def __init__(self, stream, start, size):
        self.stream, self.start, self.size = stream, start, size
        stream.seek(start)

    def seek(self, offset, whence=0):
        base = {0: self.start, 1: self.stream.tell(), 2: self.start + self.size}[whence]
        return self.stream.seek(base + offset) - self.start

    def tell(self):
        return self.stream.tell() - self.start

    def read(self, n=-1):
        return self.stream.read(n)


class Operation:
    def __init__(self, raw):
        f = fields(raw)
        self.type = f.get(1, [0])[0]
        self.data_offset = f.get(2, [0])[0]
        self.data_length = f.get(3, [0])[0]
        self.dst_extents = [(first(e, 1, 0), first(e, 2, 0)) for e in f.get(6, [])]
        self.data_sha256 = f.get(8, [None])[0]
        self.has_source = bool(f.get(4))


class Partition:
    def __init__(self, raw):
        f = fields(raw)
        self.name = f[1][0].decode()
        info = f.get(7, [b''])[0]
        self.size = first(info, 1, 0)
        self.sha256 = first(info, 2)
        self.operations = [Operation(op) for op in f.get(8, [])]


class Payload:
    def __init__(self, path):
        self.stream = open_payload(path)
        magic, version, manifest_size = struct.unpack('>4sQQ', self.stream.read(20))
        if magic != b'CrAU':
            sys.exit(f'{path}: not an OTA payload (magic {magic!r})')
        if version != 2:
            sys.exit(f'{path}: payload version {version} is not supported')
        signature_size, = struct.unpack('>I', self.stream.read(4))
        manifest = self.stream.read(manifest_size)
        self.data_start = 24 + manifest_size + signature_size
        f = fields(manifest)
        self.block_size = f.get(3, [4096])[0]
        self.minor_version = f.get(12, [0])[0]
        self.partitions = [Partition(p) for p in f.get(13, [])]

    def read_blob(self, op):
        self.stream.seek(self.data_start + op.data_offset)
        return self.stream.read(op.data_length)


def decode(op, blob):
    if op.data_sha256 and hashlib.sha256(blob).digest() != op.data_sha256:
        raise ValueError('operation data hash mismatch')
    if op.type == 0:
        return blob
    if op.type == 1:
        return bz2.decompress(blob)
    if op.type == 8:
        return lzma.decompress(blob)
    if op.type == 14:
        import zstandard  # only needed by OTAs that use it
        return zstandard.ZstdDecompressor().decompress(blob, max_output_size=1 << 31)
    raise ValueError(f'unexpected operation {OP_NAMES.get(op.type, op.type)}')


def extract(payload, part, out_path, workers):
    unsupported = {OP_NAMES.get(op.type, op.type) for op in part.operations
                   if op.type not in (0, 1, 6, 7, 8, 14) or op.has_source}
    if unsupported:
        sys.exit(f'{part.name}: delta operations {sorted(unsupported)}; Prism needs a full OTA')
    block = payload.block_size
    tmp = out_path + '.part'
    with open(tmp, 'wb') as out:
        out.truncate(part.size)  # ZERO and DISCARD blocks stay as the zeros truncate gives us
        with concurrent.futures.ThreadPoolExecutor(workers) as pool:
            pending = []
            for op in part.operations:
                if op.type in (6, 7):
                    continue
                pending.append((op, pool.submit(decode, op, payload.read_blob(op))))
                if len(pending) >= workers * 2:
                    _write(out, *pending.pop(0), block)
            for item in pending:
                _write(out, *item, block)
    if part.sha256:
        digest = hashlib.sha256()
        with open(tmp, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 24), b''):
                digest.update(chunk)
        if digest.digest() != part.sha256:
            os.remove(tmp)
            sys.exit(f'{part.name}: image hash does not match the manifest')
    os.replace(tmp, out_path)


def _write(out, op, future, block):
    data = future.result()
    pos = 0
    for start, count in op.dst_extents:
        out.seek(start * block)
        out.write(data[pos:pos + count * block])
        pos += count * block


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('payload', help='OTA zip or payload.bin')
    ap.add_argument('-o', '--out', default='work/images')
    ap.add_argument('-p', '--partitions', help='comma-separated names, or "all" (default: Android partitions)')
    ap.add_argument('--list', action='store_true', help='list partitions and exit')
    ap.add_argument('-j', '--workers', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    payload = Payload(args.payload)
    if args.list:
        for p in payload.partitions:
            kinds = sorted({OP_NAMES.get(op.type, str(op.type)) for op in p.operations})
            print(f'{p.name:16} {p.size / 2**20:9.1f} MiB  {len(p.operations):5} ops  {",".join(kinds)}')
        return
    wanted = (None if args.partitions == 'all' else
              args.partitions.split(',') if args.partitions else ANDROID_PARTITIONS)
    names = [p.name for p in payload.partitions]
    for name in wanted or []:
        if name not in names:
            sys.exit(f'no partition named {name}; the payload has {", ".join(names)}')
    os.makedirs(args.out, exist_ok=True)
    for part in payload.partitions:
        if wanted and part.name not in wanted:
            continue
        out_path = os.path.join(args.out, part.name + '.img')
        print(f'{part.name}: {part.size / 2**20:.1f} MiB -> {out_path}', flush=True)
        extract(payload, part, out_path, args.workers)
    print('done')


if __name__ == '__main__':
    main()
