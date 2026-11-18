#!/usr/bin/env python3
"""Decode arm64e chained-fixup pointer values and dump full ObjC metadata.
Fixup decoding (per mach-o/fixup-chains.h):
  ARM64E rebase:      target:43, high8:8, next:11, bind:1, auth:1
  ARM64E auth_rebase: target:32, diversity:16, addrDiv:1, key:2, next:11, bind:1, auth:1
  ARM64E bind:        ordinal:16, zero:16, addend:19, next:11, bind:1, auth:1
  ARM64E auth_bind:   ordinal:16, zero:16, diversity:16, addrDiv:1, key:2, next:11, bind:1, auth:1
  USERLAND24 bind24:      ordinal:24, zero:8, addend:19, next:11, bind:1, auth:1
  USERLAND24 auth_bind24: ordinal:24, zero:8, diversity:16, addrDiv:1, key:2, next:11, bind:1, auth:1
"""
import struct, sys

def parse_fat(path):
    with open(path, 'rb') as f:
        data = f.read()
    magic = struct.unpack('>I', data[:4])[0]
    slices = []
    if magic == 0xcafebabe:
        n = struct.unpack('>I', data[4:8])[0]
        for i in range(n):
            cputype, cpusub, off, size, align = struct.unpack('>IIIII', data[8+i*20:28+i*20])
            slices.append((cputype, cpusub, off, size))
    else:
        slices.append((0x0100000c, 0, 0, len(data)))
    return data, slices

def decode_ptr(v, fmt):
    """Return dict describing a stored 64-bit fixup value."""
    auth = (v >> 63) & 1
    bind = (v >> 62) & 1
    nxt = (v >> 51) & 0x7FF
    if auth:
        if bind:
            # auth_bind (fmt1) or auth_bind24 (fmt12)
            if fmt == 12:
                ordinal = v & 0xFFFFFF
                div = (v >> 32) & 0xFFFF
            else:
                ordinal = v & 0xFFFF
                div = (v >> 32) & 0xFFFF
            addrdiv = (v >> 48) & 1
            key = (v >> 49) & 3
            return dict(kind='auth_bind', ordinal=ordinal, diversity=div, addrDiv=addrdiv, key=key, next=nxt)
        else:
            target = v & 0xFFFFFFFF
            div = (v >> 32) & 0xFFFF
            addrdiv = (v >> 48) & 1
            key = (v >> 49) & 3
            return dict(kind='auth_rebase', va=target, diversity=div, addrDiv=addrdiv, key=key, next=nxt)
    else:
        if bind:
            if fmt == 12:
                ordinal = v & 0xFFFFFF
                addend = (v >> 32) & 0x7FFFF
            else:
                ordinal = v & 0xFFFF
                addend = (v >> 32) & 0x7FFFF
            return dict(kind='bind', ordinal=ordinal, addend=addend, next=nxt)
        else:
            target = v & ((1 << 43) - 1)
            high8 = (v >> 43) & 0xFF
            va = (high8 << 56) | target
            return dict(kind='rebase', va=va, target=target, high8=high8, next=nxt)

def parse_macho(data, off):
    _m, _ct, _cs, ftype, ncmds, _sz, _fl, _r = struct.unpack('<IIIIIIII', data[off:off+32])
    segs, sections, fixfmt = [], {}, {}
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack('<II', data[p:p+8])
        if cmd == 0x19:
            name = data[p+8:p+24].rstrip(b'\x00').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack('<QQQQ', data[p+24:p+56])
            nsects = struct.unpack('<I', data[p+64:p+68])[0]
            segs.append((name, vmaddr, vmsize, fileoff, filesize))
            sp = p + 72
            for _ in range(nsects):
                sname = data[sp:sp+16].rstrip(b'\x00').decode()
                gname = data[sp+16:sp+32].rstrip(b'\x00').decode()
                addr, sz, off2 = struct.unpack('<QQI', data[sp+32:sp+52])
                sections[(gname, sname)] = (addr, sz, off2)
                sp += 80
        elif cmd & 0x7FFFFFFF == 0x34:  # LC_DYLD_CHAINED_FIXUPS (may have REQ_DYLD bit)
            doff = struct.unpack('<I', data[p+8:p+12])[0]
            base = off + doff
            ver, starts_off = struct.unpack('<II', data[base:base+8])
            sbase = base + starts_off
            seg_count = struct.unpack('<I', data[sbase:sbase+4])[0]
            for i in range(seg_count):
                so = struct.unpack('<I', data[sbase+4+i*4:sbase+8+i*4])[0]
                if so == 0:
                    continue
                sb = sbase + so
                sz, page_size, ptr_fmt, seg_off, max_valid, page_cnt = struct.unpack('<IIHHQI', data[sb:sb+24])
                fixfmt[i] = ptr_fmt
        p += size
    return ftype, segs, sections, fixfmt

def dump(path, label):
    print(f'########## {label}')
    data, slices = parse_fat(path)
    arm64e = None
    for cputype, cpusub, off, size in slices:
        if cputype == 0x0100000c and (cpusub & 0xFF) == 2:
            arm64e = (off, size)
    if arm64e is None and len(slices) == 1 and slices[0][0] == 0x0100000c:
        arm64e = (slices[0][2], slices[0][3])  # thin arm64e file
    off, _ = arm64e
    ftype, segs, sections, fixfmt = parse_macho(data, off)
    print(f'filetype={ftype} (6=MH_DYLIB, 8=MH_BUNDLE)  fixup pointer formats per segment: {fixfmt}')
    # __DATA is segment index 2, __DATA_CONST index 1 (per llvm-objdump order)
    fmt_data = fixfmt.get(2, 1)
    fmt_const = fixfmt.get(1, 1)
    print(f'pointer_format __DATA_CONST={fmt_const}  __DATA={fmt_data}  (1=ARM64E, 12=ARM64E_USERLAND24)')

    def va_to_off(va):
        for name, vmaddr, vmsize, fileoff, filesize in segs:
            if vmaddr <= va < vmaddr + vmsize:
                return fileoff + (va - vmaddr)
        return None

    def read_va(va, n):
        fo = va_to_off(va)
        if fo is None:
            return None
        return data[off+fo:off+fo+n]

    def u64_va(va):
        r = read_va(va, 8)
        return struct.unpack('<Q', r)[0] if r else None

    def sec(name):
        return sections.get(('__DATA', name)) or sections.get(('__DATA_CONST', name))

    ii = sec('__objc_imageinfo')
    ver, flags = struct.unpack('<II', data[off+ii[2]:off+ii[2]+8])
    print(f'__objc_imageinfo version={ver} flags={flags:#x} (0x2 Swift, 0x4 Preopt, 0x8 RelMethodSel, 0x10 IsBundle, 0x20 Sim, 0x40 DyldPreopt)')

    cl = sec('__objc_classlist')
    print(f'__objc_classlist addr={cl[0]:#x} ({cl[1]//8} classes)')
    for i in range(cl[1]//8):
        v = struct.unpack('<Q', data[off+cl[2]+i*8:off+cl[2]+i*8+8])[0]
        d = decode_ptr(v, fmt_const)
        print(f'  classlist[{i}] raw={v:#018x} -> {d}')

    # walk classes
    for i in range(cl[1]//8):
        v = struct.unpack('<Q', data[off+cl[2]+i*8:off+cl[2]+i*8+8])[0]
        cva = decode_ptr(v, fmt_const).get('va')
        print(f'--- class {i} VA {cva:#x}')
        raw = read_va(cva, 40)
        isa, supercls, cache, bits, dval = struct.unpack('<QQQQQ', raw)
        print(f'  isa        {decode_ptr(isa, fmt_data)}')
        print(f'  superclass {decode_ptr(supercls, fmt_data)}')
        print(f'  cache      {decode_ptr(cache, fmt_data)}')
        print(f'  data       {decode_ptr(dval, fmt_data)}')
        dd = decode_ptr(dval, fmt_data)
        if 'va' not in dd:
            print('  (data is a bind, cannot resolve)'); continue
        rraw = read_va(dd['va'] & ~7, 72)
        if not rraw:
            print('  (class_ro_t unresolvable)'); continue
        rflags, istart, isize, res, ivarlayout, name, methods, protos, ivars, weakivar, props = struct.unpack('<IIIIQQQQQQQ', rraw)
        print(f'  class_ro_t flags={rflags:#x} (0x1 META,0x2 ROOT,0x4 CXX_STRUCTORS,0x10 HIDDEN,0x80 IS_ARC,0x100 FROM_BUNDLE,0x200 FUTURE) instanceStart={istart} instanceSize={isize}')
        nraw = read_va(decode_ptr(name, fmt_data).get('va', 0), 64)
        print(f'    name={nraw.split(bytes([0]))[0]!r}  name_ptr={decode_ptr(name, fmt_data)}')
        print(f'    baseMethods={decode_ptr(methods, fmt_data)}')
        print(f'    baseProtocols={decode_ptr(protos, fmt_data)}')
        print(f'    ivars={decode_ptr(ivars, fmt_data)}  baseProperties={decode_ptr(props, fmt_data)}')
        md = decode_ptr(methods, fmt_data)
        if 'va' in md:
            mraw = read_va(md['va'], 8)
            if mraw:
                entsize, count = struct.unpack('<II', mraw)
                print(f'    method_list entsizeAndFlags={entsize:#010x} isRelative={bool(entsize & 0x80000000)} entsize={entsize & 0xFFFF} count={count}')
                if entsize & 0x80000000:
                    for mi in range(min(count, 2)):
                        m = read_va(md['va']+8+mi*12, 12)
                        no, ty, imp = struct.unpack('<iii', m)
                        print(f'      rel method[{mi}]: name_off={no} types_off={ty} imp_off={imp}')
                else:
                    for mi in range(min(count, 2)):
                        m = read_va(md['va']+8+mi*24, 24)
                        no, ty, imp = struct.unpack('<QQQ', m)
                        print(f'      abs method[{mi}]: name={decode_ptr(no, fmt_data)} types={decode_ptr(ty, fmt_data)} imp={decode_ptr(imp, fmt_data)}')

if __name__ == '__main__':
    dump(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'DYLIB')
