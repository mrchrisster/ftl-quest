import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

for addr in [0x000000010023BE28, 0x000000010023BE48]:
    off = addr - 0x100000000
    isa, flags, str_ptr, length = struct.unpack('<QQQQ', data[off:off+32])
    str_off = str_ptr - 0x100000000
    s = data[str_off:str_off+length].decode('utf-8', errors='replace')
    print(f'CFString at 0x{addr:X}: isa=0x{isa:X}, flags=0x{flags:X}, text="{s}"')
