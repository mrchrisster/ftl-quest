import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

va = 0x1000B9E60
off = va - 0x100000000

for i in range(0, 256, 4):
    insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
    pc = va + i
    if (insn & 0x9F000000) == 0x10000000: # ADR
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000: imm -= 0x200000
        target = pc + imm
        s_off = target - 0x100000000
        if 0 <= s_off < len(data):
            end = data.find(b"\x00", s_off)
            s = data[s_off:min(end, s_off + 80)].decode("utf-8", errors="replace")
            print(f"  0x{pc:X}: ADR target 0x{target:X} -> \"{s}\"")
    elif (insn & 0x9F000000) == 0x90000000: # ADRP
        immlo = (insn >> 29) & 0x3
        immhi = (insn >> 5) & 0x7FFFF
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000: imm -= 0x200000000
        base = pc & ~0xFFF
        target = base + imm
        # Look ahead for ADD or LDR
        next_insn = struct.unpack("<I", data[off + i + 4 : off + i + 8])[0]
        if (next_insn & 0xFF800000) == 0x91000000: # ADD immediate
            add_imm = (next_insn >> 10) & 0xFFF
            full_target = target + add_imm
            s_off = full_target - 0x100000000
            if 0 <= s_off < len(data):
                end = data.find(b"\x00", s_off)
                s = data[s_off:min(end, s_off + 80)].decode("utf-8", errors="replace")
                print(f"  0x{pc:X}: ADRP+ADD target 0x{full_target:X} -> \"{s}\"")
