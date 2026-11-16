import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

stub_idx = stubs.index('_AudioUnitSetProperty')
stub_va = 0x1001E918C + stub_idx * 12

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\sections.json') as f:
    sects = json.load(f)

text_sect = [s for s in sects if s['sectname'] == '__text'][0]
t_off = text_sect['offset']
t_sz = text_sect['size']
t_va = text_sect['addr']

print(f"_AudioUnitSetProperty stub is at 0x{stub_va:X}")

for i in range(0, t_sz, 4):
    pc = t_va + i
    insn = struct.unpack("<I", data[t_off + i : t_off + i + 4])[0]
    if (insn & 0xFC000000) == 0x94000000:
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        if target == stub_va:
            print(f"BL _AudioUnitSetProperty at 0x{pc:016X}")
            # Disassemble preceding 15 instructions
            start = max(0, i - 60)
            for j in range(start, i + 8, 4):
                curr_pc = t_va + j
                curr_insn = struct.unpack("<I", data[t_off + j : t_off + j + 4])[0]
                print(f"    0x{curr_pc:X}: 0x{curr_insn:08X}")
