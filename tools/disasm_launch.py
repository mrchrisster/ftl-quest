import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

start_va = 0x000000010019A550
off = start_va - 0x100000000
raw = data[off : off + 256]

print(f"Disassembly trace of [SILAppDelegate application:didFinishLaunchingWithOptions:] at VA 0x{start_va:X}:")

def decode_insn(val, pc):
    # Check bl / b
    if (val & 0xFC000000) == 0x94000000:
        imm = val & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        target = pc + imm * 4
        return f"bl 0x{target:016X}"
    elif (val & 0xFC000000) == 0x14000000:
        imm = val & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        target = pc + imm * 4
        return f"b 0x{target:016X}"
    elif (val & 0x9F000000) == 0x10000000: # ADR
        immlo = (val >> 29) & 0x3
        immhi = (val >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000:
            imm -= 0x200000
        target = pc + imm
        rd = val & 0x1F
        return f"adr x{rd}, 0x{target:016X}"
    elif (val & 0x9F000000) == 0x90000000: # ADRP
        immlo = (val >> 29) & 0x3
        immhi = (val >> 5) & 0x7FFFF
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000:
            imm -= 0x200000000
        base = pc & ~0xFFF
        target = base + imm
        rd = val & 0x1F
        return f"adrp x{rd}, 0x{target:016X}"
    elif val == 0xD65F03C0:
        return "ret"
    elif val == 0xD503201F:
        return "nop"
    return f"0x{val:08X}"

for i in range(0, len(raw), 4):
    insn = struct.unpack("<I", raw[i:i+4])[0]
    pc = start_va + i
    dec = decode_insn(insn, pc)
    print(f"0x{pc:016X} : {dec}")
    if dec == "ret":
        break
