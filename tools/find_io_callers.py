import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)
stubs = d['__TEXT,__stubs']

targets = ['_open', '_pread', '_fread', '_fstat', '_stat', '_access', '_lseek', '_read']
target_map = {}
for t in targets:
    if t in stubs:
        idx = stubs.index(t)
        target_map[0x1001E918C + idx*12] = t

print("Target I/O stubs:", {f"0x{k:X}": v for k, v in target_map.items()})

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\sections.json') as f:
    sects = json.load(f)

text_sect = [s for s in sects if s['sectname'] == '__text'][0]
t_off = text_sect['offset']
t_sz = text_sect['size']
t_va = text_sect['addr']

counts = {v: 0 for v in targets}

for i in range(0, t_sz, 4):
    pc = t_va + i
    insn = struct.unpack("<I", data[t_off + i : t_off + i + 4])[0]
    if (insn & 0xFC000000) == 0x94000000:
        imm = insn & 0x03FFFFFF
        if imm & 0x02000000: imm -= 0x04000000
        target = pc + imm * 4
        if target in target_map:
            name = target_map[target]
            counts[name] += 1
            print(f"  BL {name:8} at 0x{pc:016X}")

print("\nCall counts:", counts)
