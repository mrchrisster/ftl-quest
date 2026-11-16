import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

va = 0x1000F4BEC
off = va - 0x100000000

for i in range(0, 300, 4):
    pc = va + i
    insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
    extra = ""
    if (insn & 0xFC000000) == 0x94000000: # BL
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        if 0x1001E918C <= target < 0x1001E918C + len(stubs)*12:
            s_idx = (target - 0x1001E918C) // 12
            extra = f" -> {stubs[s_idx]}"
        else:
            extra = f" -> 0x{target:X}"
    elif (insn & 0xFF000000) == 0x71000000: # CMP immediate
        rn = (insn >> 5) & 0x1F
        imm12 = (insn >> 10) & 0xFFF
        extra = f" [cmp w{rn}, #0x{imm12:X}]"
    print(f"  0x{pc:016X} : 0x{insn:08X}{extra}")
