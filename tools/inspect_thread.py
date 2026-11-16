import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

func_va = 0x000000010012FAD0
off = func_va - 0x100000000

for i in range(0, 256, 4):
    insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
    pc = func_va + i
    if (insn & 0xFC000000) == 0x94000000: # BL
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        stub_name = ""
        if 0x1001E918C <= target < 0x1001E918C + len(stubs)*12:
            s_idx = (target - 0x1001E918C) // 12
            stub_name = f" -> {stubs[s_idx]}"
        print(f"  0x{pc:X}: BL 0x{target:X}{stub_name}")
