"""Minimal reader for Android binary XML (compiled AndroidManifest.xml)."""
import struct

# Framework attribute resource IDs, for manifests whose attribute names were stripped.
ATTR_IDS = {0x01010003: 'name', 0x01010006: 'permission', 0x0101000b: 'sharedUserId', 0x0101000d: 'persistent',
            0x01010010: 'exported', 0x01010011: 'process', 0x0101021b: 'versionCode',
            0x0101021c: 'versionName', 0x0101020c: 'minSdkVersion', 0x01010270: 'targetSdkVersion',
            0x01010000: 'theme', 0x01010001: 'label', 0x01010002: 'icon', 0x01010018: 'enabled'}


class Element:
    def __init__(self, tag, attrs):
        self.tag, self.attrs, self.children = tag, attrs, []

    def iter(self, tag=None):
        if tag is None or self.tag == tag:
            yield self
        for child in self.children:
            yield from child.iter(tag)

    def get(self, name, default=None):
        return self.attrs.get(name, default)


def _strings(buf, off):
    count, _styles, flags, strings_start, _ = struct.unpack_from('<IIIII', buf, off + 8)
    utf8 = flags & 0x100
    offsets = struct.unpack_from(f'<{count}I', buf, off + 28)
    base = off + strings_start
    out = []
    for o in offsets:
        p = base + o
        if utf8:
            p += 2 if buf[p] & 0x80 else 1  # UTF-16 length, unused
            n = buf[p]
            if n & 0x80:
                n = (n & 0x7F) << 8 | buf[p + 1]
                p += 1
            p += 1
            out.append(buf[p:p + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', buf, p)[0]
            p += 2
            if n & 0x8000:
                n = (n & 0x7FFF) << 16 | struct.unpack_from('<H', buf, p)[0]
                p += 2
            out.append(buf[p:p + n * 2].decode('utf-16le', 'replace'))
    return out


def _value(strings, raw, data_type, data):
    if data_type == 0x03 or (raw != 0xFFFFFFFF and data_type in (0x03, 0x00)):
        return strings[raw] if raw < len(strings) else ''
    if data_type == 0x12:
        return data != 0
    if data_type in (0x10, 0x11):
        return data if data < 0x80000000 else data - (1 << 32)
    if data_type == 0x01:
        return f'@0x{data:08x}'
    return raw if raw != 0xFFFFFFFF else data


def parse(buf):
    """Returns the root Element of a binary XML document."""
    if struct.unpack_from('<H', buf, 0)[0] != 0x0003:
        raise ValueError('not binary XML')
    strings, res_ids, stack, root = [], [], [], None
    pos = struct.unpack_from('<H', buf, 2)[0]
    while pos < len(buf):
        kind, header_size, size = struct.unpack_from('<HHI', buf, pos)
        if size == 0:
            break
        if kind == 0x0001:
            strings = _strings(buf, pos)
        elif kind == 0x0180:
            res_ids = list(struct.unpack_from(f'<{(size - header_size) // 4}I', buf, pos + header_size))
        elif kind == 0x0102:
            body = pos + header_size
            _ns, name, attr_start, attr_size, attr_count = struct.unpack_from('<IIHHH', buf, body)
            attrs = {}
            for i in range(attr_count):
                a = body + attr_start + i * attr_size
                _ans, aname, raw, _vsize, _res0, dtype, data = struct.unpack_from('<IIIHBBI', buf, a)
                key = strings[aname] if aname < len(strings) else ''
                if not key and aname < len(res_ids):
                    key = ATTR_IDS.get(res_ids[aname], f'0x{res_ids[aname]:08x}')
                attrs[key] = _value(strings, raw, dtype, data)
            element = Element(strings[name], attrs)
            if stack:
                stack[-1].children.append(element)
            else:
                root = element
            stack.append(element)
        elif kind == 0x0103:
            stack.pop()
        pos += size
    return root
