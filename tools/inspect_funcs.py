import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

def inspect_function_references(func_va):
    print(f"\n=== Inspecting function at VA 0x{func_va:016X} ===")
    off = func_va - 0x100000000
    for i in range(0, 128, 4):
        insn = struct.unpack("<I", data[off + i : off + i + 4])[0]
        pc = func_va + i
        # Check for ADR / ADRP
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
        elif (insn & 0xFC000000) == 0x94000000: # BL
            imm = insn & 0x03FFFFFF
            if imm & 0x02000000: imm -= 0x04000000
            target = pc + imm * 4
            print(f"  0x{pc:X}: BL 0x{target:X}")

inspect_function_references(0x000000010011D108)
inspect_function_references(0x000000010012FAD0)
