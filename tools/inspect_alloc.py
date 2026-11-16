import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

for va in [0x1001BE780, 0x1001BE9A4]:
    print(f"\n--- Disassembly at 0x{va:X} ---")
    off = va - 0x100000000
    for i in range(0, 64, 4):
        insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
        pc = va + i
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
        print(f"  0x{pc:016X} : 0x{insn:08X}{extra}")
