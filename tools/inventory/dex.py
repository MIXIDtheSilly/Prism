"""Minimal dex reader: class names, native method signatures and the string table."""
import struct


def _uleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, pos


class Dex:
    def __init__(self, buf):
        if buf[:4] != b'dex\n':
            raise ValueError('not a dex file')
        self.buf = buf
        (self.string_count, string_off, type_count, type_off, proto_count, proto_off, _fc, _fo,
         method_count, method_off, class_count, class_off) = struct.unpack_from('<12I', buf, 0x38)
        self._string_off = string_off
        self.types = struct.unpack_from(f'<{type_count}I', buf, type_off)
        self.protos = [struct.unpack_from('<III', buf, proto_off + 12 * i) for i in range(proto_count)]
        self.methods = [struct.unpack_from('<HHI', buf, method_off + 8 * i) for i in range(method_count)]
        self.class_defs = [struct.unpack_from('<8I', buf, class_off + 32 * i) for i in range(class_count)]
        self._cache = {}

    def string(self, index):
        cached = self._cache.get(index)
        if cached is None:
            off = struct.unpack_from('<I', self.buf, self._string_off + 4 * index)[0]
            _length, start = _uleb(self.buf, off)
            end = self.buf.index(b'\0', start)
            cached = self._cache[index] = self.buf[start:end].decode('utf-8', 'surrogateescape')
        return cached

    def strings(self):
        return (self.string(i) for i in range(self.string_count))

    def type_name(self, index):
        return self.string(self.types[index])

    def method_signature(self, index):
        class_idx, proto_idx, name_idx = self.methods[index]
        _shorty, return_type, params_off = self.protos[proto_idx]
        params = ''
        if params_off:
            n = struct.unpack_from('<I', self.buf, params_off)[0]
            params = ''.join(self.type_name(t) for t in struct.unpack_from(f'<{n}H', self.buf, params_off + 4))
        return f'{self.type_name(class_idx)}->{self.string(name_idx)}({params}){self.type_name(return_type)}'

    def class_names(self):
        return [self.type_name(c[0]) for c in self.class_defs]

    def methods_of_classes(self):
        """Yields (class name, method index, access flags, code offset) for every defined method."""
        for class_def in self.class_defs:
            data_off = class_def[6]
            if not data_off:
                continue
            pos = data_off
            sizes = []
            for _ in range(4):
                value, pos = _uleb(self.buf, pos)
                sizes.append(value)
            for _ in range(sizes[0] + sizes[1]):  # fields
                _, pos = _uleb(self.buf, pos)
                _, pos = _uleb(self.buf, pos)
            class_name = self.type_name(class_def[0])
            for count in sizes[2:]:  # direct, then virtual methods
                index = 0
                for _ in range(count):
                    diff, pos = _uleb(self.buf, pos)
                    flags, pos = _uleb(self.buf, pos)
                    code, pos = _uleb(self.buf, pos)
                    index += diff
                    yield class_name, index, flags, code

    def native_methods(self):
        """Yields the signature of every method declared native."""
        for _cls, index, flags, _code in self.methods_of_classes():
            if flags & 0x100:
                yield self.method_signature(index)

    def load_library_calls(self):
        """Yields (class name, library) for each System.loadLibrary("<constant>") in the code.

        Matches a const-string followed within a few code units by invoke-static of loadLibrary,
        which is how javac compiles a call with a constant argument."""
        targets = {i for i, (c, p, n) in enumerate(self.methods)
                   if self.string(n) == 'loadLibrary' and self.type_name(c) == 'Ljava/lang/System;'}
        if not targets:
            return
        for cls, _index, _flags, code in self.methods_of_classes():
            if not code:
                continue
            units = struct.unpack_from('<I', self.buf, code + 12)[0]
            insns = struct.unpack_from(f'<{units}H', self.buf, code + 16)
            for i in range(units - 1):
                op = insns[i] & 0xFF
                if op == 0x1A:  # const-string vAA, string@BBBB
                    string = insns[i + 1]
                elif op == 0x1B and i + 2 < units:  # const-string/jumbo
                    string = insns[i + 1] | insns[i + 2] << 16
                else:
                    continue
                for j in range(i + 2, min(i + 8, units - 1)):
                    if insns[j] & 0xFF == 0x71 and insns[j + 1] in targets:  # invoke-static
                        if string < self.string_count:
                            yield cls, self.string(string)
                        break
