import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

def print_ptr(va):
    off = va - 0x100000000
    ptr = struct.unpack('<Q', data[off:off+8])[0]
    p_off = ptr - 0x100000000
    if 0 <= p_off < len(data):
        end = data.find(b'\x00', p_off)
        s = data[p_off:end].decode('utf-8', errors='replace')
        print(f"0x{va:X} -> 0x{ptr:X} (\"{s}\")")
    else:
        print(f"0x{va:X} -> 0x{ptr:X}")

print_ptr(0x10023E000 + 0x220)
print_ptr(0x10023D000 + 0xC98)
print_ptr(0x10023E000 + 0x68)
print_ptr(0x10023D000 + 0xD88)
print_ptr(0x10023E000 + 0x60)
