import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

cmd_off = 32
ncmds = struct.unpack('<I', data[16:20])[0]

for _ in range(ncmds):
    cmd, cmdsz = struct.unpack('<II', data[cmd_off:cmd_off+8])
    if cmd in (0x22, 0x22 | 0x80000000): # LC_DYLD_INFO(_ONLY)
        fields = struct.unpack('<10I', data[cmd_off+8:cmd_off+48])
        print("LC_DYLD_INFO_ONLY:")
        print(f"  Rebase    : off=0x{fields[0]:X}, size=0x{fields[1]:X} ({fields[1]} B)")
        print(f"  Bind      : off=0x{fields[2]:X}, size=0x{fields[3]:X} ({fields[3]} B)")
        print(f"  Weak Bind : off=0x{fields[4]:X}, size=0x{fields[5]:X} ({fields[5]} B)")
        print(f"  Lazy Bind : off=0x{fields[6]:X}, size=0x{fields[7]:X} ({fields[7]} B)")
        print(f"  Export    : off=0x{fields[8]:X}, size=0x{fields[9]:X} ({fields[9]} B)")
    cmd_off += cmdsz
