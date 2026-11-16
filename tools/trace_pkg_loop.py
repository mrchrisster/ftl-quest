import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

va = 0x1000F4D80
off = va - 0x100000000

print(f"=== Disassembly of PKG entry loop at 0x{va:X} ===")
for i in range(0, 160, 4):
    pc = va + i
    insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
    extra = ""
    if (insn & 0xFC000000) == 0x94000000:
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        extra = f" -> 0x{target:X}"
    print(f"  0x{pc:016X} : 0x{insn:08X}{extra}")
