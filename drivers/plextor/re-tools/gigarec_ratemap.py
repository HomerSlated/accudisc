#!/usr/bin/env python3
"""Decode the GigaRec rate-code jump table from PTPXL.exe.

Confirms GigaRec = vendor opcode 0xE9, page 0x06, and prints the
rate-code -> multiplier map used in PROTOCOL.md. Point PTPXL at the
read-only reference binary.
"""
import struct, sys

PTPXL = sys.argv[1] if len(sys.argv) > 1 else \
    "../../../reference/Plextor/PTPXL/PTPXL.exe"
data = open(PTPXL, "rb").read()

def v2f(v):  # .text VMA -> file offset
    return v - 0x401000 + 0x1000
def rd_str(vma):  # .rdata VMA -> C string
    f = vma - 0x840000 + 0x440000
    return data[f:data.find(b"\x00", f)].decode("latin1")

bmap, jt = v2f(0x42f838), v2f(0x42f810)          # byte-map, pointer table
bytes_map = [data[bmap + i] for i in range(0x85)]
jtable = [struct.unpack_from("<I", data, jt + 4 * i)[0]
          for i in range(max(bytes_map) + 1)]

# Each case handler pushes its rate string before a common tail; the push
# immediate is the string VMA. Recover it by reading the `push imm32`
# (opcode 0x68) at/after the handler entry.
def handler_string(vma):
    f = v2f(vma)
    for p in range(f, f + 24):
        if data[p] == 0x68:  # push imm32
            s = struct.unpack_from("<I", data, p + 1)[0]
            if 0x844000 <= s <= 0x845000:
                return rd_str(s)
    return "?"

print("GigaRec = opcode 0xE9, page 0x06; rate code -> label:")
for code in range(0x85):
    h = jtable[bytes_map[code]]
    s = handler_string(h)
    if "x" in s:  # a real rate, not the default handler
        print("  0x%02x -> %s" % (code, s))
