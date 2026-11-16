import struct
import json
import sys

# Dyld Info Opcodes
REBASE_OPCODE_MASK = 0xF0
REBASE_IMMEDIATE_MASK = 0x0F
REBASE_OPCODE_DONE = 0x00
REBASE_OPCODE_SET_TYPE_IMM = 0x10
REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20
REBASE_OPCODE_ADD_ADDR_ULEB = 0x30
REBASE_OPCODE_ADD_ADDR_IMM_SCALED = 0x40
REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50
REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB = 0x60
REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x70

BIND_OPCODE_MASK = 0xF0
BIND_IMMEDIATE_MASK = 0x0F
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20
BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_ADDEND_SLEB = 0x60
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_ADD_ADDR_ULEB = 0x80
BIND_OPCODE_DO_BIND = 0x90
BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0
BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0
BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0

def read_uleb128(data, off):
    res = 0
    shift = 0
    while True:
        b = data[off]
        off += 1
        res |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
    return res, off

def read_sleb128(data, off):
    res = 0
    shift = 0
    while True:
        b = data[off]
        off += 1
        res |= (b & 0x7F) << shift
        shift += 7
        if (b & 0x80) == 0:
            if (b & 0x40) != 0:
                res |= -(1 << shift)
            break
    return res, off

def parse_rebases(rebase_data, segments):
    rebases = []
    off = 0
    seg_idx = 0
    seg_off = 0
    rebase_type = 1 # REBASE_TYPE_POINTER
    ptr_size = 8
    
    while off < len(rebase_data):
        b = rebase_data[off]
        off += 1
        opcode = b & REBASE_OPCODE_MASK
        imm = b & REBASE_IMMEDIATE_MASK
        
        if opcode == REBASE_OPCODE_DONE:
            break
        elif opcode == REBASE_OPCODE_SET_TYPE_IMM:
            rebase_type = imm
        elif opcode == REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg_idx = imm
            seg_off, off = read_uleb128(rebase_data, off)
        elif opcode == REBASE_OPCODE_ADD_ADDR_ULEB:
            val, off = read_uleb128(rebase_data, off)
            seg_off += val
        elif opcode == REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            seg_off += imm * ptr_size
        elif opcode == REBASE_OPCODE_DO_REBASE_IMM_TIMES:
            for _ in range(imm):
                va = segments[seg_idx]["vmaddr"] + seg_off
                rebases.append(va)
                seg_off += ptr_size
        elif opcode == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
            va = segments[seg_idx]["vmaddr"] + seg_off
            rebases.append(va)
            val, off = read_uleb128(rebase_data, off)
            seg_off += ptr_size + val
        elif opcode == REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count, off = read_uleb128(rebase_data, off)
            skip, off = read_uleb128(rebase_data, off)
            for _ in range(count):
                va = segments[seg_idx]["vmaddr"] + seg_off
                rebases.append(va)
                seg_off += ptr_size + skip

    return rebases

def parse_binds(bind_data, segments):
    binds = []
    off = 0
    seg_idx = 0
    seg_off = 0
    symbol_name = ""
    addend = 0
    dylib_ordinal = 0
    bind_type = 1
    ptr_size = 8

    while off < len(bind_data):
        b = bind_data[off]
        off += 1
        opcode = b & BIND_OPCODE_MASK
        imm = b & BIND_IMMEDIATE_MASK

        if opcode == BIND_OPCODE_DONE:
            break
        elif opcode == BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            dylib_ordinal = imm
        elif opcode == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            dylib_ordinal, off = read_uleb128(bind_data, off)
        elif opcode == BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            end = bind_data.find(b"\x00", off)
            symbol_name = bind_data[off:end].decode("utf-8", errors="replace")
            off = end + 1
        elif opcode == BIND_OPCODE_SET_TYPE_IMM:
            bind_type = imm
        elif opcode == BIND_OPCODE_SET_ADDEND_SLEB:
            addend, off = read_sleb128(bind_data, off)
        elif opcode == BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            seg_idx = imm
            seg_off, off = read_uleb128(bind_data, off)
        elif opcode == BIND_OPCODE_ADD_ADDR_ULEB:
            val, off = read_uleb128(bind_data, off)
            seg_off += val
        elif opcode == BIND_OPCODE_DO_BIND:
            va = segments[seg_idx]["vmaddr"] + seg_off
            binds.append({"va": va, "symbol": symbol_name, "addend": addend, "dylib": dylib_ordinal})
            seg_off += ptr_size
        elif opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            va = segments[seg_idx]["vmaddr"] + seg_off
            binds.append({"va": va, "symbol": symbol_name, "addend": addend, "dylib": dylib_ordinal})
            val, off = read_uleb128(bind_data, off)
            seg_off += ptr_size + val
        elif opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            va = segments[seg_idx]["vmaddr"] + seg_off
            binds.append({"va": va, "symbol": symbol_name, "addend": addend, "dylib": dylib_ordinal})
            seg_off += ptr_size + imm * ptr_size

    return binds

def main():
    path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
    with open(path, 'rb') as f:
        f.seek(0x230000)
        data = f.read(2445152)

    # Load segments
    cmd_off = 32
    ncmds = struct.unpack('<I', data[16:20])[0]
    segments = []
    dyld_info = {}

    for _ in range(ncmds):
        cmd, cmdsz = struct.unpack('<II', data[cmd_off:cmd_off+8])
        if cmd == 0x19:
            segname = data[cmd_off+8:cmd_off+24].split(b'\x00')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack('<QQQQ', data[cmd_off+24:cmd_off+56])
            segments.append({"name": segname, "vmaddr": vmaddr, "vmsize": vmsize, "fileoff": fileoff, "filesize": filesize})
        elif cmd in (0x22, 0x22 | 0x80000000):
            fields = struct.unpack('<10I', data[cmd_off+8:cmd_off+48])
            dyld_info = {
                "rebase_off": fields[0], "rebase_size": fields[1],
                "bind_off": fields[2], "bind_size": fields[3],
                "lazy_bind_off": fields[6], "lazy_bind_size": fields[7]
            }
        cmd_off += cmdsz

    rebase_data = data[dyld_info["rebase_off"] : dyld_info["rebase_off"] + dyld_info["rebase_size"]]
    rebases = parse_rebases(rebase_data, segments)
    print(f"Parsed {len(rebases)} internal pointer rebases.")

    bind_data = data[dyld_info["bind_off"] : dyld_info["bind_off"] + dyld_info["bind_size"]]
    binds = parse_binds(bind_data, segments)
    print(f"Parsed {len(binds)} non-lazy dynamic symbol binds.")

    lazy_data = data[dyld_info["lazy_bind_off"] : dyld_info["lazy_bind_off"] + dyld_info["lazy_bind_size"]]
    lazy_binds = parse_binds(lazy_data, segments)
    print(f"Parsed {len(lazy_binds)} lazy symbol binds.")

    out = {
        "num_rebases": len(rebases),
        "num_binds": len(binds),
        "num_lazy_binds": len(lazy_binds),
        "rebases": [hex(r) for r in rebases[:50]],
        "binds": [{"va": hex(b["va"]), "symbol": b["symbol"]} for b in binds[:50]],
        "lazy_binds": [{"va": hex(b["va"]), "symbol": b["symbol"]} for b in lazy_binds[:50]]
    }

    with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\dyld_relocations.json', 'w') as f:
        json.dump(out, f, indent=2)

    print("Saved dyld_relocations.json successfully.")

if __name__ == "__main__":
    main()
