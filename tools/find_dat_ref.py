import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\sections.json') as f:
    sects = json.load(f)

cstr_sect = [s for s in sects if s['sectname'] == '__cstring'][0]
off = cstr_sect['offset']
sz = cstr_sect['size']

ftl_dat_off = data.find(b"ftl.dat\x00", off)
ftl_dat_va = 0x100000000 + ftl_dat_off
print(f"ftl.dat is at VA 0x{ftl_dat_va:016X} (file offset 0x{ftl_dat_off:X})")

# Search __text for references to ftl_dat_va
text_sect = [s for s in sects if s['sectname'] == '__text'][0]
t_off = text_sect['offset']
t_sz = text_sect['size']
t_va = text_sect['addr']

print(f"Scanning __text (0x{t_va:X} - 0x{t_va+t_sz:X}) for references...")

for i in range(0, t_sz - 8, 4):
    pc = t_va + i
    insn = struct.unpack("<I", data[t_off + i : t_off + i + 4])[0]
    
    # Check ADR
    if (insn & 0x9F000000) == 0x10000000:
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000: imm -= 0x200000
        target = pc + imm
        if target == ftl_dat_va:
            print(f"Found ADR reference at 0x{pc:016X}")
            
    # Check ADRP
    elif (insn & 0x9F000000) == 0x90000000:
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000: imm -= 0x200000000
        base = pc & ~0xFFF
        target = base + imm
        if (target & ~0xFFF) == (ftl_dat_va & ~0xFFF):
            next_insn = struct.unpack("<I", data[t_off + i + 4 : t_off + i + 8])[0]
            if (next_insn & 0xFF800000) == 0x91000000: # ADD
                add_imm = (next_insn >> 10) & 0xFFF
                if target + add_imm == ftl_dat_va:
                    print(f"Found ADRP+ADD reference at 0x{pc:016X}")
