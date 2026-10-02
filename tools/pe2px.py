#!/usr/bin/env python3
"""Check a wlink-built PE DLL against what Jemm's JLOAD requires of a JLM,
then patch the signature PE -> PX.  Usage: pe2px.py FILE.DLL"""
import struct, sys

p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
lf = struct.unpack_from('<I', d, 0x3C)[0]
sig = bytes(d[lf:lf+4])
mach, nsec, _, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', d, lf+4)
opt = lf + 24
entry = struct.unpack_from('<I', d, opt+16)[0]
imagebase = struct.unpack_from('<I', d, opt+28)[0]
ndir = struct.unpack_from('<I', d, opt+92)[0]
dirs = [struct.unpack_from('<II', d, opt+96+8*i) for i in range(ndir)]
secs = []
for i in range(nsec):
    o = opt + optsz + 40*i
    name = d[o:o+8].rstrip(b'\0').decode()
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o+8)
    secs.append((name, va, vsz, rptr, rsz))
def rva2off(rva):
    for n, va, vsz, rp, rs in secs:
        if va <= rva < va + max(vsz, rs):
            return rp + rva - va
    raise ValueError(hex(rva))
print(f"sig {sig!r} machine {mach:#x} chars {chars:#x} entry {entry:#x} base {imagebase:#x}")
for s in secs:
    print("  section %-8s va %#06x vsz %#06x raw %#06x rsz %#06x" % s)
ok = True
if mach != 0x14C: print("FAIL: not i386"); ok = False
if chars & 1: print("FAIL: relocations stripped"); ok = False
if dirs[1][0]: print("FAIL: has imports"); ok = False
if not dirs[5][0]: print("FAIL: no base relocations"); ok = False
if not dirs[0][0]:
    print("FAIL: no exports"); ok = False
else:
    e = rva2off(dirs[0][0])
    nfunc, nname, afunc, aname = struct.unpack_from('<IIII', d, e+20)
    f0 = struct.unpack_from('<I', d, rva2off(afunc))[0]
    n0 = struct.unpack_from('<I', d, rva2off(aname))[0] if nname else 0
    nm = d[rva2off(n0):].split(b'\0')[0].decode() if n0 else '?'
    ddb = rva2off(f0)
    print(f"export[0] {nm} rva {f0:#x}: DDB id {struct.unpack_from('<H', d, ddb+6)[0]:#06x} "
          f"name {bytes(d[ddb+12:ddb+20])!r}")
if not ok:
    sys.exit(1)
if sig == b'PE\0\0':
    d[lf:lf+4] = b'PX\0\0'
    open(p, 'wb').write(d)
    print("patched PE -> PX")
elif sig == b'PX\0\0':
    print("already PX")
