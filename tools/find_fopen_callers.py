import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\sections.json') as f:
    sects = json.load(f)

text_sect = [s for s in sects if s['sectname'] == '__text'][0]
t_off = text_sect['offset']
t_sz = text_sect['size']
t_va = text_sect['addr']

fopen_stub = 0x1001E9750
print(f"Scanning for BL 0x{fopen_stub:X} (_fopen)...")

fopen_callers = []
for i in range(0, t_sz, 4):
    pc = t_va + i
    insn = struct.unpack("<I", data[t_off + i : t_off + i + 4])[0]
    if (insn & 0xFC000000) == 0x94000000:
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        if target == fopen_stub:
            fopen_callers.append(pc)
            print(f"  BL _fopen at 0x{pc:016X}")

print(f"Total fopen calls in game binary: {len(fopen_callers)}")
