import struct
import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

# Find sections
def get_slice_data(va, length):
    off = va - 0x100000000
    if 0 <= off < len(data):
        return data[off : min(off + length, len(data))]
    return b""

def read_cstring(va):
    off = va - 0x100000000
    if 0 <= off < len(data):
        end = data.find(b"\x00", off)
        if end != -1:
            return data[off:end].decode("utf-8", errors="replace")
    return ""

def read_u64(va):
    raw = get_slice_data(va, 8)
    if len(raw) == 8:
        return struct.unpack("<Q", raw)[0]
    return 0

def read_u32(va):
    raw = get_slice_data(va, 4)
    if len(raw) == 4:
        return struct.unpack("<I", raw)[0]
    return 0

# __DATA,__objc_classlist is at 0x10023BE68 (64 bytes = 8 classes)
# struct objc_class_t { uint64_t isa; uint64_t superclass; uint64_t cache; uint64_t vtable; uint64_t data; }
classlist_va = 0x10023BE68
num_classes = 64 // 8

classes_info = []

for i in range(num_classes):
    cls_ptr = read_u64(classlist_va + i * 8)
    if cls_ptr == 0:
        continue
    
    superclass_ptr = read_u64(cls_ptr + 8)
    ro_ptr = read_u64(cls_ptr + 32) # class_ro_t
    
    flags = read_u32(ro_ptr)
    instanceStart = read_u32(ro_ptr + 4)
    instanceSize = read_u32(ro_ptr + 8)
    name_ptr = read_u64(ro_ptr + 24)
    baseMethodList_ptr = read_u64(ro_ptr + 32)
    baseProtocols_ptr = read_u64(ro_ptr + 40)
    ivars_ptr = read_u64(ro_ptr + 48)

    class_name = read_cstring(name_ptr)
    
    # Parse method list
    methods = []
    if baseMethodList_ptr != 0:
        entsize_and_flags = read_u32(baseMethodList_ptr)
        entsize = entsize_and_flags & 0xFFFF
        count = read_u32(baseMethodList_ptr + 4)
        m_curr = baseMethodList_ptr + 8
        for m_i in range(count):
            m_name_ptr = read_u64(m_curr)
            m_types_ptr = read_u64(m_curr + 8)
            m_imp_ptr = read_u64(m_curr + 16)
            m_name = read_cstring(m_name_ptr)
            m_types = read_cstring(m_types_ptr)
            methods.append({
                "selector": m_name,
                "types": m_types,
                "imp_va": f"0x{m_imp_ptr:016X}"
            })
            m_curr += entsize

    classes_info.append({
        "class_name": class_name,
        "class_va": f"0x{cls_ptr:016X}",
        "instance_size": instanceSize,
        "methods": methods
    })

print(f"Parsed {len(classes_info)} Objective-C classes from __objc_classlist:")
for c in classes_info:
    print(f"\n[Class] {c['class_name']} (Size: {c['instance_size']} bytes, {len(c['methods'])} methods)")
    for m in c['methods']:
        print(f"    - {m['selector']} -> IMP: {m['imp_va']} ({m['types']})")

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\objc_classes_detailed.json', 'w') as f:
    json.dump(classes_info, f, indent=2)
