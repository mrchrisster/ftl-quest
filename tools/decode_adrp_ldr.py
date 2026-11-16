import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

# Disassemble 0x100116508 to 0x100116578
va = 0x100116508
off = va - 0x100000000

for i in range(0, 112, 4):
    pc = va + i
    insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
    extra = ""
    # ADRP
    if (insn & 0x9F000000) == 0x90000000:
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000: imm -= 0x200000000
        base = pc & ~0xFFF
        target = base + imm
        rd = insn & 0x1F
        extra = f" [ADRP x{rd}, 0x{target:X}]"
    # LDR 64-bit unsigned immediate: 0xF9400000
    elif (insn & 0xFFC00000) == 0xF9400000:
        rn = (insn >> 5) & 0x1F
        rt = insn & 0x1F
        imm12 = (insn >> 10) & 0xFFF
        extra = f" [LDR x{rt}, [x{rn}, #0x{imm12*8:X}]]"
    print(f"  0x{pc:X} : 0x{insn:08X}{extra}")
