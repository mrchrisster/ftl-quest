import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000) # ARM64 slice
    data = f.read(2445152)

magic = struct.unpack('<I', data[:4])[0]
cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack('<IIIIIII', data[4:32])

cmd_off = 32
symtab = {}
dysym = {}
sections = []

for _ in range(ncmds):
    cmd, cmdsz = struct.unpack('<II', data[cmd_off:cmd_off+8])
    if cmd == 0x19: # LC_SEGMENT_64
        segname = data[cmd_off+8:cmd_off+24].split(b'\x00')[0].decode()
        nsects = struct.unpack('<I', data[cmd_off+64:cmd_off+68])[0]
        s_off = cmd_off + 72
        for _ in range(nsects):
            s_name = data[s_off:s_off+16].split(b'\x00')[0].decode()
            s_seg = data[s_off+16:s_off+32].split(b'\x00')[0].decode()
            addr, sz, offset, align, reloff, nreloc, s_flags, r1, r2, r3 = struct.unpack('<QQIIIIIIII', data[s_off+32:s_off+80])
            sections.append({'name': f'{s_seg},{s_name}', 'addr': addr, 'size': sz, 'offset': offset, 'flags': s_flags, 'r1': r1, 'r2': r2})
            s_off += 80
    elif cmd == 0x2: # LC_SYMTAB
        symtab['symoff'], symtab['nsyms'], symtab['stroff'], symtab['strsize'] = struct.unpack('<IIII', data[cmd_off+8:cmd_off+24])
    elif cmd == 0xB: # LC_DYSYMTAB
        fields = struct.unpack('<18I', data[cmd_off+8:cmd_off+80])
        dysym['indirectsymoff'] = fields[12]
        dysym['nindirectsyms'] = fields[13]
    cmd_off += cmdsz

strtab = data[symtab['stroff'] : symtab['stroff'] + symtab['strsize']]
symbols = []
for i in range(symtab['nsyms']):
    off = symtab['symoff'] + i * 16
    n_strx, n_type, n_sect, n_desc, n_value = struct.unpack('<IBBHQ', data[off:off+16])
    end = strtab.find(b'\x00', n_strx)
    name = strtab[n_strx:end].decode('utf-8', errors='replace') if end != -1 else ''
    symbols.append(name)

ind_syms = struct.unpack(f'<{dysym["nindirectsyms"]}I', data[dysym['indirectsymoff'] : dysym['indirectsymoff'] + dysym['nindirectsyms'] * 4])

print(f"Total symbols: {len(symbols)}")
print(f"Total indirect symbols: {len(ind_syms)}")

report = {}
for s in sections:
    name = s['name']
    if any(k in name for k in ['la_symbol_ptr', 'got', 'stubs']):
        idx = s['r1']
        stride = 8 if 'ptr' in name or 'got' in name else 12
        count = s['size'] // stride
        entries = []
        for i in range(count):
            if idx + i < len(ind_syms):
                sym_idx = ind_syms[idx + i]
                sym_name = symbols[sym_idx] if sym_idx < len(symbols) else f'0x{sym_idx:X}'
                entries.append(sym_name)
        report[name] = entries
        print(f"Section {name}: {count} entries")

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json', 'w') as f:
    json.dump(report, f, indent=2)

print("Saved indirect_symbols.json")
