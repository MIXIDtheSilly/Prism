"""Minimal ELF reader: architecture, SONAME and DT_NEEDED."""
import struct

MACHINES = {0x03: 'x86', 0x28: 'arm', 0x3E: 'x86_64', 0xB7: 'arm64'}


def read(path):
    """Returns {'arch', 'needed', 'soname'} or None if the file isn't ELF."""
    with open(path, 'rb') as f:
        head = f.read(64)
        if head[:4] != b'\x7fELF':
            return None
        is64 = head[4] == 2
        machine = struct.unpack_from('<H', head, 18)[0]
        info = {'arch': MACHINES.get(machine, hex(machine)), 'needed': [], 'soname': None}
        if is64:
            phoff, = struct.unpack_from('<Q', head, 32)
            phentsize, phnum = struct.unpack_from('<HH', head, 54)
        else:
            phoff, = struct.unpack_from('<I', head, 28)
            phentsize, phnum = struct.unpack_from('<HH', head, 42)
        f.seek(phoff)
        table = f.read(phentsize * phnum)
        loads, dynamic = [], None
        for i in range(phnum):
            if is64:
                p_type, _flags, offset, vaddr, _paddr, filesz = struct.unpack_from('<IIQQQQ', table, i * phentsize)
            else:
                p_type, offset, vaddr, _paddr, filesz = struct.unpack_from('<IIIII', table, i * phentsize)
            if p_type == 1:
                loads.append((vaddr, offset, filesz))
            elif p_type == 2:
                dynamic = (offset, filesz)
        if not dynamic:
            return info
        f.seek(dynamic[0])
        raw = f.read(dynamic[1])
        size = 16 if is64 else 8
        entries = [struct.unpack_from('<qQ' if is64 else '<iI', raw, i) for i in range(0, len(raw) - size + 1, size)]
        strtab = next((v for t, v in entries if t == 5), None)
        if strtab is None:
            return info
        strtab_off = next((off + strtab - va for va, off, sz in loads if va <= strtab < va + sz), None)
        if strtab_off is None:
            return info

        def string(offset):
            f.seek(strtab_off + offset)
            data = f.read(256)
            return data[:data.index(b'\0')].decode(errors='replace') if b'\0' in data else data.decode(errors='replace')

        for tag, value in entries:
            if tag == 0:
                break
            if tag == 1:
                info['needed'].append(string(value))
            elif tag == 14:
                info['soname'] = string(value)
        return info
