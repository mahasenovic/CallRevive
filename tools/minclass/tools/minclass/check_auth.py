#!/usr/bin/env python3
"""Gate: every ObjC class in an arm64e dylib/bundle must have
PAC-authenticated isa/data (auth_rebase) and superclass
(auth_bind, or nil for a root class).

This is the check that decides whether a build is safe to install:
libobjc readClass authenticates those three pointers at load time,
and an unsigned one traps with EXC_BREAKPOINT — exactly what
killed every L1ghtmann-built ObjC dylib (docs/READCLASS-CRASH.md).

Usage: check_auth.py <mach-o> [label]
Exits 0 when all classes pass, 1 otherwise.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from objc_dump2 import parse_fat, parse_macho, decode_ptr  # noqa: E402


def check(path, label):
    data, slices = parse_fat(path)
    arm64e = None
    for cputype, cpusub, off, size in slices:
        if cputype == 0x0100000C and (cpusub & 0xFF) == 2:
            arm64e = (off, size)
    if arm64e is None and len(slices) == 1 and slices[0][0] == 0x0100000C:
        arm64e = (slices[0][2], slices[0][3])  # thin arm64e file
    if arm64e is None:
        print(f'{label}: FAIL - no arm64e slice found')
        return False
    off, _ = arm64e
    _ftype, segs, sections, fixfmt = parse_macho(data, off)
    # __DATA is segment index 2, __DATA_CONST index 1 (llvm-objdump order)
    fmt_data = fixfmt.get(2, 1)
    fmt_const = fixfmt.get(1, 1)

    def va_to_off(va):
        for _name, vmaddr, vmsize, fileoff, _filesize in segs:
            if vmaddr <= va < vmaddr + vmsize:
                return fileoff + (va - vmaddr)
        return None

    def read_va(va, n):
        fo = va_to_off(va)
        if fo is None:
            return None
        return data[off + fo:off + fo + n]

    def sec(name):
        return (sections.get(('__DATA', name))
                or sections.get(('__DATA_CONST', name)))

    cl = sec('__objc_classlist')
    if cl is None:
        print(f'{label}: no __objc_classlist (pure-C binary?) - nothing to gate')
        return True

    ok = True
    for i in range(cl[1] // 8):
        v = struct.unpack('<Q', data[off + cl[2] + i * 8:off + cl[2] + i * 8 + 8])[0]
        cva = decode_ptr(v, fmt_const).get('va')
        raw = read_va(cva, 40)
        if raw is None:
            print(f'{label}: class {i}: FAIL - cannot read class struct')
            ok = False
            continue
        isa, supercls, _cache, _bits, dval = struct.unpack('<QQQQQ', raw)
        isa_k = decode_ptr(isa, fmt_data).get('kind')
        data_k = decode_ptr(dval, fmt_data).get('kind')
        sup_k = decode_ptr(supercls, fmt_data).get('kind')
        # Class name, for a readable report.
        name = '?'
        dd = decode_ptr(dval, fmt_data)
        if 'va' in dd:
            rraw = read_va(dd['va'] & ~7, 72)
            if rraw:
                nm = struct.unpack('<IIIIQQQQQQQ', rraw)[5]
                nraw = read_va(decode_ptr(nm, fmt_data).get('va', 0), 64)
                if nraw:
                    name = nraw.split(b'\x00')[0].decode(errors='replace')
        sup_ok = (supercls == 0) or (sup_k == 'auth_bind')
        cls_ok = (isa_k == 'auth_rebase' and data_k == 'auth_rebase' and sup_ok)
        print(f'{label}: class {name}: isa={isa_k} data={data_k} '
              f'superclass={sup_k} -> {"OK" if cls_ok else "FAIL"}')
        ok = ok and cls_ok
    if ok:
        print(f'{label}: PASS - all class pointers PAC-authenticated')
    else:
        print(f'{label}: FAIL - unsigned class pointers (readClass will crash)')
    return ok


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage: check_auth.py <mach-o> [label]')
        sys.exit(2)
    label = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
    sys.exit(0 if check(sys.argv[1], label) else 1)
