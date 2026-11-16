#!/usr/bin/env python3
"""
tools/analyze_macho.py - Comprehensive ARM64 Mach-O Binary Analyzer for FTL iOS Port
Parses Mach-O 64-bit headers, Fat/Universal binaries, Load Commands, Symbols,
Imports, Framework dependencies, Objective-C metadata, and C++ vtable/RTTI.
Zero external dependencies required (pure standard library).
"""

import sys
import os
import struct
import json
import re
from typing import Dict, List, Tuple, Any, Optional

# Mach-O Constants
MH_MAGIC_64 = 0xFEEDFACF
MH_CIGAM_64 = 0xCFFAEDFE
FAT_MAGIC = 0xCAFEBABE
FAT_CIGAM = 0xBEBAFECA
FAT_MAGIC_64 = 0xCAFEBABF
FAT_CIGAM_64 = 0xBFBAFECA

CPU_TYPE_ARM64 = 0x0100000C
CPU_SUBTYPE_ARM64_ALL = 0x00000000
CPU_SUBTYPE_ARM64_V8 = 0x00000001
CPU_SUBTYPE_ARM64E = 0x00000002

LC_REQ_DYLD = 0x80000000
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B
LC_LOAD_DYLIB = 0x0C
LC_LOAD_WEAK_DYLIB = (0x18 | LC_REQ_DYLD)
LC_REEXPORT_DYLIB = (0x1F | LC_REQ_DYLD)
LC_ID_DYLIB = 0x0D
LC_LOAD_DYLINKER = 0x0E
LC_ROUTINES_64 = 0x1A
LC_MAIN = (0x28 | LC_REQ_DYLD)
LC_UNIXTHREAD = 0x05
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = (0x22 | LC_REQ_DYLD)
LC_FUNCTION_STARTS = 0x26
LC_DATA_IN_CODE = 0x29
LC_BUILD_VERSION = 0x32
LC_VERSION_MIN_IPHONEOS = 0x25

N_STAB = 0xE0
N_PEXT = 0x10
N_TYPE = 0x0E
N_EXT = 0x01

N_UNDF = 0x0
N_ABS = 0x2
N_SECT = 0xE
N_INDR = 0xA


class MachOParser:
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.data = b""
        self.offset = 0
        self.size = 0
        self.is_64 = True
        self.endian = "<"
        
        self.header = {}
        self.segments = []
        self.sections = []
        self.sections_by_name = {}
        self.dylibs = []
        self.entry_point = None
        self.symtab_cmd = None
        self.dysymtab_cmd = None
        self.dyld_info_cmd = None
        self.symbols = []
        self.imported_symbols = []
        self.exported_symbols = []
        self.objc_classes = []
        self.objc_methods = []
        self.vtables = []
        self.rtti_types = []

    def load(self):
        with open(self.filepath, "rb") as f:
            self.data = f.read()
        self.size = len(self.data)
        self._locate_arm64_slice()
        self._parse_header()
        self._parse_load_commands()
        self._parse_symbols()
        self._parse_objc_metadata()
        self._scan_cpp_structures()

    def _locate_arm64_slice(self):
        if len(self.data) < 4:
            raise ValueError("File too small to be a Mach-O")

        magic = struct.unpack(">I", self.data[:4])[0]
        if magic in (FAT_MAGIC, FAT_CIGAM):
            endian = ">" if magic == FAT_MAGIC else "<"
            nfat_arch = struct.unpack(f"{endian}I", self.data[4:8])[0]
            print(f"[*] Detected Universal Fat binary with {nfat_arch} architectures.")
            found_arm64 = False
            for i in range(nfat_arch):
                arch_off = 8 + i * 20
                cputype, cpusubtype, offset, size, align = struct.unpack(
                    f"{endian}IIIII", self.data[arch_off : arch_off + 20]
                )
                if cputype == CPU_TYPE_ARM64:
                    self.offset = offset
                    self.size = size
                    found_arm64 = True
                    print(f"[*] Selected ARM64 slice at offset 0x{offset:X} (size: {size} bytes).")
                    break
            if not found_arm64:
                raise ValueError("No ARM64 slice found in Universal Fat binary.")
        else:
            self.offset = 0

    def _parse_header(self):
        magic = struct.unpack("<I", self.data[self.offset : self.offset + 4])[0]
        if magic == MH_MAGIC_64:
            self.endian = "<"
            self.is_64 = True
        elif magic == MH_CIGAM_64:
            self.endian = ">"
            self.is_64 = True
        else:
            raise ValueError(f"Unsupported Mach-O magic: 0x{magic:08X} (expected ARM64 64-bit)")

        fields = struct.unpack(
            f"{self.endian}IIIIIII", self.data[self.offset + 4 : self.offset + 32]
        )
        self.header = {
            "cputype": fields[0],
            "cpusubtype": fields[1],
            "filetype": fields[2],
            "ncmds": fields[3],
            "sizeofcmds": fields[4],
            "flags": fields[5],
            "reserved": fields[6],
        }

    def _parse_load_commands(self):
        curr = self.offset + 32
        for _ in range(self.header["ncmds"]):
            cmd, cmdsize = struct.unpack(f"{self.endian}II", self.data[curr : curr + 8])
            raw_cmd = self.data[curr : curr + cmdsize]

            if cmd == LC_SEGMENT_64:
                seg = self._parse_segment_64(raw_cmd)
                self.segments.append(seg)
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB):
                dylib_path = self._parse_dylib_command(raw_cmd)
                self.dylibs.append({
                    "path": dylib_path,
                    "weak": (cmd == LC_LOAD_WEAK_DYLIB)
                })
            elif cmd == LC_MAIN:
                entryoff, stacksize = struct.unpack(f"{self.endian}QQ", raw_cmd[8:24])
                self.entry_point = {"type": "LC_MAIN", "offset": entryoff, "stacksize": stacksize}
            elif cmd == LC_UNIXTHREAD:
                # ARM64 thread state: 16-byte header, then x0-x30, sp, pc, pstate
                # flavor 6 = ARM_THREAD_STATE64
                flavor, count = struct.unpack(f"{self.endian}II", raw_cmd[8:16])
                if flavor == 6:
                    pc = struct.unpack(f"{self.endian}Q", raw_cmd[16 + 32 * 8 : 16 + 33 * 8])[0]
                    self.entry_point = {"type": "LC_UNIXTHREAD", "pc": pc}
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack(f"{self.endian}IIII", raw_cmd[8:24])
                self.symtab_cmd = {
                    "symoff": symoff,
                    "nsyms": nsyms,
                    "stroff": stroff,
                    "strsize": strsize,
                }
            elif cmd == LC_DYSYMTAB:
                fields = struct.unpack(f"{self.endian}18I", raw_cmd[8:80])
                self.dysymtab_cmd = {
                    "ilocalsym": fields[0],
                    "nlocalsym": fields[1],
                    "iextdefsym": fields[2],
                    "nextdefsym": fields[3],
                    "iundefsym": fields[4],
                    "nundefsym": fields[5],
                    "tocoff": fields[6],
                    "ntoc": fields[7],
                    "modtaboff": fields[8],
                    "nmodtab": fields[9],
                    "extrefsymoff": fields[10],
                    "nextrefsyms": fields[11],
                    "indirectsymoff": fields[12],
                    "nindirectsyms": fields[13],
                    "extreloff": fields[14],
                    "nextrel": fields[15],
                    "locreloff": fields[16],
                    "nlocrel": fields[17],
                }
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                fields = struct.unpack(f"{self.endian}10I", raw_cmd[8:48])
                self.dyld_info_cmd = {
                    "rebase_off": fields[0],
                    "rebase_size": fields[1],
                    "bind_off": fields[2],
                    "bind_size": fields[3],
                    "weak_bind_off": fields[4],
                    "weak_bind_size": fields[5],
                    "lazy_bind_off": fields[6],
                    "lazy_bind_size": fields[7],
                    "export_off": fields[8],
                    "export_size": fields[9],
                }

            curr += cmdsize

    def _parse_segment_64(self, raw: bytes) -> Dict[str, Any]:
        segname = raw[8:24].split(b"\x00")[0].decode("ascii", errors="replace")
        vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags = struct.unpack(
            f"{self.endian}QQQQIIII", raw[24:72]
        )
        seg = {
            "name": segname,
            "vmaddr": vmaddr,
            "vmsize": vmsize,
            "fileoff": fileoff,
            "filesize": filesize,
            "maxprot": maxprot,
            "initprot": initprot,
            "nsects": nsects,
            "flags": flags,
            "sections": [],
        }

        curr_sect = 72
        for _ in range(nsects):
            s_raw = raw[curr_sect : curr_sect + 80]
            sectname = s_raw[0:16].split(b"\x00")[0].decode("ascii", errors="replace")
            s_segname = s_raw[16:32].split(b"\x00")[0].decode("ascii", errors="replace")
            addr, size, offset, align, reloff, nreloc, s_flags, r1, r2, r3 = struct.unpack(
                f"{self.endian}QQIIIIIIII", s_raw[32:80]
            )
            sect = {
                "sectname": sectname,
                "segname": s_segname,
                "addr": addr,
                "size": size,
                "offset": offset,
                "align": align,
                "reloff": reloff,
                "nreloc": nreloc,
                "flags": s_flags,
            }
            seg["sections"].append(sect)
            self.sections.append(sect)
            full_key = f"{s_segname},{sectname}"
            self.sections_by_name[full_key] = sect
            curr_sect += 80

        return seg

    def _parse_dylib_command(self, raw: bytes) -> str:
        stroff = struct.unpack(f"{self.endian}I", raw[8:12])[0]
        name_bytes = raw[stroff:].split(b"\x00")[0]
        return name_bytes.decode("utf-8", errors="replace")

    def _parse_symbols(self):
        if not self.symtab_cmd:
            return

        stroff = self.offset + self.symtab_cmd["stroff"]
        strsize = self.symtab_cmd["strsize"]
        strtab = self.data[stroff : stroff + strsize]

        symoff = self.offset + self.symtab_cmd["symoff"]
        nsyms = self.symtab_cmd["nsyms"]

        # nlist_64: uint32 n_strx, uint8 n_type, uint8 n_sect, uint16 n_desc, uint64 n_value (16 bytes)
        for i in range(nsyms):
            entry_bytes = self.data[symoff + i * 16 : symoff + (i + 1) * 16]
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack(
                f"{self.endian}IBBHQ", entry_bytes
            )

            if n_strx >= len(strtab):
                sym_name = ""
            else:
                end_str = strtab.find(b"\x00", n_strx)
                sym_name = strtab[n_strx:end_str].decode("utf-8", errors="replace") if end_str != -1 else ""

            is_stab = (n_type & N_STAB) != 0
            if is_stab:
                continue

            type_field = n_type & N_TYPE
            is_external = (n_type & N_EXT) != 0

            sym_info = {
                "name": sym_name,
                "value": n_value,
                "sect": n_sect,
                "type": n_type,
                "desc": n_desc,
                "external": is_external,
            }

            self.symbols.append(sym_info)

            if type_field == N_UNDF:
                self.imported_symbols.append(sym_info)
            elif is_external and type_field == N_SECT:
                self.exported_symbols.append(sym_info)

    def _parse_objc_metadata(self):
        meth_sect = self.sections_by_name.get("__TEXT,__objc_methname")
        if meth_sect and meth_sect["size"] > 0:
            off = self.offset + meth_sect["offset"]
            meth_data = self.data[off : off + meth_sect["size"]]
            for raw_str in meth_data.split(b"\x00"):
                if raw_str:
                    s = raw_str.decode("utf-8", errors="replace")
                    if s:
                        self.objc_methods.append(s)

        class_sect = self.sections_by_name.get("__TEXT,__objc_classname")
        if class_sect and class_sect["size"] > 0:
            off = self.offset + class_sect["offset"]
            class_data = self.data[off : off + class_sect["size"]]
            for raw_str in class_data.split(b"\x00"):
                if raw_str:
                    s = raw_str.decode("utf-8", errors="replace")
                    if s:
                        self.objc_classes.append(s)

    def _scan_cpp_structures(self):
        # Scan symbols for C++ mangled names, vtables (_ZTV), typeinfo (_ZTI)
        for sym in self.symbols:
            name = sym["name"]
            if name.startswith("_ZTV") or name.startswith("__ZTV"):
                self.vtables.append({"name": name, "address": sym["value"]})
            elif name.startswith("_ZTI") or name.startswith("__ZTI"):
                self.rtti_types.append({"name": name, "address": sym["value"]})

    def categorize_imports(self) -> Dict[str, List[str]]:
        categories = {
            "POSIX / C Lib / Math": [],
            "C++ Standard Library": [],
            "Apple UIKit / UIWindow": [],
            "Apple OpenGLES / CAEAGL": [],
            "Apple OpenAL / Audio": [],
            "Apple Foundation / System": [],
            "GameKit / StoreKit / Services": [],
            "Other / Uncategorized": [],
        }

        for sym in self.imported_symbols:
            name = sym["name"]
            stripped = name.lstrip("_")

            if stripped.startswith("std::") or name.startswith("_ZSt") or name.startswith("__cxa") or name.startswith("__gxx"):
                categories["C++ Standard Library"].append(name)
            elif any(stripped.startswith(p) for p in ("UI", "NSClassFromString", "UIApplication", "UITouch", "UIView")):
                categories["Apple UIKit / UIWindow"].append(name)
            elif any(stripped.startswith(p) for p in ("EAGL", "gl", "glTex", "glBind", "glDraw", "glClear", "glEnable")):
                categories["Apple OpenGLES / CAEAGL"].append(name)
            elif any(stripped.startswith(p) for p in ("al", "alc", "Audio", "AudioQueue", "ExtAudioFile")):
                categories["Apple OpenAL / Audio"].append(name)
            elif any(stripped.startswith(p) for p in ("CF", "NS", "objc_", "_NSConcrete")):
                categories["Apple Foundation / System"].append(name)
            elif any(stripped.startswith(p) for p in ("GK", "SK", "GameCenter")):
                categories["GameKit / StoreKit / Services"].append(name)
            elif any(stripped.startswith(p) for p in ("printf", "malloc", "free", "sin", "cos", "sqrt", "fopen", "fclose", "fread", "fwrite", "pthread", "stat", "access")):
                categories["POSIX / C Lib / Math"].append(name)
            else:
                categories["Other / Uncategorized"].append(name)

        return categories

    def generate_report(self, out_dir: str):
        os.makedirs(out_dir, exist_ok=True)

        summary = {
            "file": os.path.basename(self.filepath),
            "size": self.size,
            "cputype": f"0x{self.header.get('cputype', 0):X} (ARM64)",
            "filetype": self.header.get("filetype"),
            "entry_point": self.entry_point,
            "num_segments": len(self.segments),
            "num_sections": len(self.sections),
            "num_dylibs": len(self.dylibs),
            "num_symbols": len(self.symbols),
            "num_imported_symbols": len(self.imported_symbols),
            "num_exported_symbols": len(self.exported_symbols),
            "num_objc_classes": len(self.objc_classes),
            "num_objc_methods": len(self.objc_methods),
            "num_vtables": len(self.vtables),
            "num_rtti_types": len(self.rtti_types),
            "dylibs": self.dylibs,
        }

        with open(os.path.join(out_dir, "triage_summary.json"), "w") as f:
            json.dump(summary, f, indent=2)

        with open(os.path.join(out_dir, "sections.json"), "w") as f:
            json.dump(self.sections, f, indent=2)

        categorized = self.categorize_imports()
        with open(os.path.join(out_dir, "categorized_imports.json"), "w") as f:
            json.dump(categorized, f, indent=2)

        with open(os.path.join(out_dir, "symbols_imported.json"), "w") as f:
            json.dump([s["name"] for s in self.imported_symbols], f, indent=2)

        with open(os.path.join(out_dir, "symbols_exported.json"), "w") as f:
            json.dump([s["name"] for s in self.exported_symbols], f, indent=2)

        with open(os.path.join(out_dir, "objc_metadata.json"), "w") as f:
            json.dump({
                "classes": self.objc_classes,
                "methods": self.objc_methods
            }, f, indent=2)

        with open(os.path.join(out_dir, "vtables.json"), "w") as f:
            json.dump(self.vtables, f, indent=2)

        print(f"\n========================================================")
        print(f" Mach-O Triage Summary: {os.path.basename(self.filepath)}")
        print(f"========================================================")
        print(f" Slice Size        : {self.size} bytes ({self.size / (1024*1024):.2f} MB)")
        print(f" Dynamic Libraries : {len(self.dylibs)} frameworks/dylibs")
        for d in self.dylibs:
            w_flag = " (WEAK)" if d["weak"] else ""
            print(f"   -> {d['path']}{w_flag}")
        print(f" Entry Point       : {self.entry_point}")
        print(f" Total Symbols     : {len(self.symbols)}")
        print(f"   - Undefined/Imp : {len(self.imported_symbols)}")
        print(f"   - Defined/Export: {len(self.exported_symbols)}")
        print(f" ObjC Classes      : {len(self.objc_classes)}")
        print(f" ObjC Selectors    : {len(self.objc_methods)}")
        print(f" C++ VTables Found : {len(self.vtables)}")
        print(f" C++ RTTI Types    : {len(self.rtti_types)}")
        print(f" Output written to : {out_dir}")
        print(f"========================================================\n")


def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_macho.py <path_to_ftl_macho_binary> [output_report_dir]")
        print("Example: python analyze_macho.py ../workspace/inputs/FTL ../workspace/reports")
        sys.exit(1)

    binary_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(binary_path), "reports")

    if not os.path.exists(binary_path):
        print(f"[-] Error: file not found: {binary_path}")
        sys.exit(1)

    parser = MachOParser(binary_path)
    try:
        parser.load()
        parser.generate_report(out_dir)
    except Exception as e:
        print(f"[-] Parsing failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
