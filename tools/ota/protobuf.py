"""Just enough protobuf wire-format decoding to read OTA manifests without the protobuf package."""


def _varint(buf, i):
    result = shift = 0
    while True:
        byte = buf[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, i


def fields(buf):
    """Decodes one message into {field number: [values]}. Varints come back as ints,
    length-delimited fields as bytes (nested messages are decoded on demand)."""
    out = {}
    i = 0
    while i < len(buf):
        key, i = _varint(buf, i)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, i = _varint(buf, i)
        elif wire == 1:
            value, i = buf[i:i + 8], i + 8
        elif wire == 2:
            length, i = _varint(buf, i)
            value, i = buf[i:i + length], i + length
        elif wire == 5:
            value, i = buf[i:i + 4], i + 4
        else:
            raise ValueError(f'unsupported wire type {wire}')
        out.setdefault(number, []).append(value)
    return out


def first(buf, number, default=None):
    values = fields(buf).get(number)
    return values[0] if values else default

