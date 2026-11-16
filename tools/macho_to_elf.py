#!/usr/bin/env python3
"""
tools/macho_to_elf.py - Mach-O ARM64 to Android ELF64 Shared Library Converter
Extracts the __TEXT and __DATA segments of the unencrypted ARM64 Mach-O executable
and converts them into a standard Android-loadable ELF64 shared object (libftl_lifted.so).
Zero external dependencies (pure Python standard library).
"""

import sys
import os
import struct

# ELF Constants
EI_MAG0 = 0x7F
EI_MAG1 = ord('E')
EI_MAG2 = ord('L')
EI_MAG3 = ord('F')
ELFCLASS64 = 2
ELFDATA2LSB = 1
EV_CURRENT = 1
ELFOSABI_NONE = 0
ET_DYN = 3           # Shared object file
EM_AARCH64 = 183     # ARM AARCH64

PT_NULL = 0
PT_LOAD = 1
PT_DYNAMIC = 2

PF_X = 1
PF_W = 2
PF_R = 4

def align_up(val, align):
    return (val + align - 1) & ~(align - 1)

def convert_macho_to_elf(macho_path, elf_out_path):
    print(f"[*] Reading Mach-O binary from: {macho_path}")
    with open(macho_path, "rb") as f:
        data = f.read()

    # Locate ARM64 slice if universal fat binary
    magic = struct.unpack(">I", data[:4])[0]
    slice_off = 0
    slice_size = len(data)
    if magic in (0xCAFEBABE, 0xBEBAFECA):
        nfat = struct.unpack(">I", data[4:8])[0]
        for i in range(nfat):
            cputype, cpusubtype, off, sz, align = struct.unpack(">IIIII", data[8+i*20 : 8+(i+1)*20])
            if cputype == 0x0100000C: # ARM64
                slice_off = off
                slice_size = sz
                break

    slice_data = data[slice_off : slice_off + slice_size]
    print(f"[*] Parsing ARM64 slice ({slice_size} bytes)...")

    # Read segments
    ncmds = struct.unpack("<I", slice_data[16:20])[0]
    cmd_ptr = 32
    text_seg = None
    data_seg = None

    for _ in range(ncmds):
        cmd, cmdsz = struct.unpack("<II", slice_data[cmd_ptr : cmd_ptr + 8])
        if cmd == 0x19: # LC_SEGMENT_64
            segname = slice_data[cmd_ptr+8 : cmd_ptr+24].split(b"\x00")[0].decode()
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags = struct.unpack(
                "<QQQQIIII", slice_data[cmd_ptr+24 : cmd_ptr+72]
            )
            seg_info = {
                "name": segname,
                "vmaddr": vmaddr,
                "vmsize": vmsize,
                "fileoff": fileoff,
                "filesize": filesize,
                "flags": initprot
            }
            if segname == "__TEXT":
                text_seg = seg_info
            elif segname == "__DATA":
                data_seg = seg_info
        cmd_ptr += cmdsz

    if not text_seg or not data_seg:
        print("[-] Error: Missing __TEXT or __DATA segment in Mach-O")
        return False

    print(f"[*] Mach-O Segments:")
    print(f"    __TEXT: vmaddr=0x{text_seg['vmaddr']:X}, size=0x{text_seg['vmsize']:X}")
    print(f"    __DATA: vmaddr=0x{data_seg['vmaddr']:X}, size=0x{data_seg['vmsize']:X}")

    # Build ELF64 Shared Library
    # Page size = 64KB for Android 15 / Quest OS compliance
    PAGE_SIZE = 0x10000

    # Section 0: Null
    # Phdr 0: PT_LOAD for __TEXT (R-X)
    # Phdr 1: PT_LOAD for __DATA (RW-)
    
    ehdr_size = 64
    phdr_size = 56
    num_phdrs = 2

    text_file_off = PAGE_SIZE
    text_size = text_seg["filesize"]
    text_vmsize = align_up(text_seg["vmsize"], PAGE_SIZE)

    data_file_off = align_up(text_file_off + text_size, PAGE_SIZE)
    data_size = data_seg["filesize"]
    data_vmsize = align_up(data_seg["vmsize"], PAGE_SIZE)

    total_file_size = data_file_off + data_size
    elf_bytes = bytearray(total_file_size)

    # 1. ELF Header (Elf64_Ehdr)
    # e_ident: 16 bytes
    elf_bytes[0:4] = bytes([EI_MAG0, EI_MAG1, EI_MAG2, EI_MAG3])
    elf_bytes[4] = ELFCLASS64
    elf_bytes[5] = ELFDATA2LSB
    elf_bytes[6] = EV_CURRENT
    elf_bytes[7] = ELFOSABI_NONE
    
    struct.pack_into(
        "<HHIQQQIHHHHHH", elf_bytes, 16,
        ET_DYN,             # e_type
        EM_AARCH64,         # e_machine
        EV_CURRENT,         # e_version
        0,                  # e_entry
        ehdr_size,          # e_phoff (immediately following Ehdr)
        0,                  # e_shoff (no section headers needed for runtime load)
        0,                  # e_flags
        ehdr_size,          # e_ehsize
        phdr_size,          # e_phentsize
        num_phdrs,          # e_phnum
        0,                  # e_shentsize
        0,                  # e_shnum
        0                   # e_shstrndx
    )

    # 2. Program Headers (Elf64_Phdr)
    # Phdr 0: __TEXT (PT_LOAD, PF_R | PF_X)
    struct.pack_into(
        "<IIQQQQQQ", elf_bytes, ehdr_size,
        PT_LOAD,
        PF_R | PF_X,
        text_file_off,      # p_offset
        text_seg["vmaddr"], # p_vaddr
        text_seg["vmaddr"], # p_paddr
        text_size,          # p_filesz
        text_vmsize,        # p_memsz
        PAGE_SIZE           # p_align
    )

    # Phdr 1: __DATA (PT_LOAD, PF_R | PF_W)
    struct.pack_into(
        "<IIQQQQQQ", elf_bytes, ehdr_size + phdr_size,
        PT_LOAD,
        PF_R | PF_W,
        data_file_off,      # p_offset
        data_seg["vmaddr"], # p_vaddr
        data_seg["vmaddr"], # p_paddr
        data_size,          # p_filesz
        data_vmsize,        # p_memsz
        PAGE_SIZE           # p_align
    )

    # 3. Copy Segments
    elf_bytes[text_file_off : text_file_off + text_size] = slice_data[text_seg["fileoff"] : text_seg["fileoff"] + text_size]
    elf_bytes[data_file_off : data_file_off + data_size] = slice_data[data_seg["fileoff"] : data_seg["fileoff"] + data_size]

    with open(elf_out_path, "wb") as f_out:
        f_out.write(elf_bytes)

    print(f"[+] Successfully converted Mach-O -> ELF64: {elf_out_path}")
    print(f"    ELF File Size: {len(elf_bytes)} bytes ({len(elf_bytes)/(1024*1024):.2f} MB)")
    print(f"    Target Arch  : AArch64 (arm64-v8a)")
    return True

def main():
    macho_in = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL"
    elf_out = sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\libftl_lifted.so"
    
    if not os.path.exists(macho_in):
        print(f"[-] Input file not found: {macho_in}")
        sys.exit(1)

    convert_macho_to_elf(macho_in, elf_out)

if __name__ == "__main__":
    main()
