import zipfile
import struct

ipa_path = r'C:\code\ftl-quest\FTL_1.6.21_ios_8.0.ipa'

with zipfile.ZipFile(ipa_path, 'r') as z:
    with z.open('Payload/FTL.app/ftl.dat') as f:
        magic = f.read(4)
        if magic != b"PKG\n":
            print("Invalid magic!")
            exit(1)
        
        # In FTL 1.6 PKG format:
        # uint32 version / header_size
        # uint32 num_files
        header_sz, num_files = struct.unpack(">II", f.read(8))
        print(f"PKG\\n format: header_sz=0x{header_sz:X}, num_files={num_files}")
        
        # Read index table
        # Each entry: uint32 name_len, name (or hash), uint32 offset, uint32 size
