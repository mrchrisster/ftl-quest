import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

ref_va = 0x000000010011424C
start_va = ref_va - 64

print(f"=== Disassembly of dat loader around 0x{ref_va:X} ===")
for i in range(0, 160, 4):
    pc = start_va + i
    off = pc - 0x100000000
    insn = struct.unpack("<I", data[off : off + 4])[0]
    
    extra = ""
    if pc == ref_va:
        extra = " <-- REFERENCES \"ftl.dat\""
    elif (insn & 0xFC000000) == 0x94000000: # BL
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        if 0x1001E918C <= target < 0x1001E918C + len(stubs)*12:
            s_idx = (target - 0x1001E918C) // 12
            extra = f" -> {stubs[s_idx]}"
        else:
            extra = f" -> 0x{target:X}"
    elif (insn & 0x9F000000) == 0x10000000: # ADR
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000: imm -= 0x200000
        target = pc + imm
        s_off = target - 0x100000000
        if 0 <= s_off < len(data):
            end = data.find(b"\x00", s_off)
            s = data[s_off:min(end, s_off + 40)].decode("utf-8", errors="replace")
            extra = f" [ADR 0x{target:X} -> \"{s}\"]"

    print(f"  0x{pc:016X} : 0x{insn:08X}{extra}")
