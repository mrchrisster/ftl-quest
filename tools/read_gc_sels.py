import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

# Check ADRP+LDR references in 0x100116508 - 0x100116574
# 0x100116508: adrp x8, 0x10023E000; ldr x0, [x8, #0x110]
# 0x100116514: adrp x8, 0x10023D000; ldr x1, [x8, #0x648]
# 0x100116524: adrp x8, 0x10023E000; ldr x1, [x8, #0x350]
# 0x100116534: adrp x8, 0x10023D000; ldr x1, [x8, #0x6C8]

def read_sel_at(va):
    off = va - 0x100000000
    sel_ptr = struct.unpack('<Q', data[off:off+8])[0]
    str_off = sel_ptr - 0x100000000
    end = data.find(b'\x00', str_off)
    return data[str_off:end].decode()

print("Selector 1:", read_sel_at(0x10023D000 + 0x648))
print("Selector 2:", read_sel_at(0x10023E000 + 0x350))
print("Selector 3:", read_sel_at(0x10023D000 + 0x6C8))
print("Selector 4:", read_sel_at(0x10023E000 + 0x115))
