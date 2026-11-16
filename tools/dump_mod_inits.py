import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

mod_init_va = 0x10022C958
mod_init_sz = 0x178
count = mod_init_sz // 8
off = mod_init_va - 0x100000000

print(f"=== {count} Static Initializers in __DATA,__mod_init_func ===")
for i in range(count):
    target_va = struct.unpack("<Q", data[off + i*8 : off + (i+1)*8])[0]
    print(f"  [{i:02d}] 0x{target_va:016X}")
