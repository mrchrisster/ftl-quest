import json

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\sections.json') as f:
    sects = json.load(f)

cstr_sect = [s for s in sects if s['sectname'] == '__cstring'][0]
off = cstr_sect['offset']
sz = cstr_sect['size']

raw_strings = data[off : off + sz].split(b'\x00')
strings = [s.decode('utf-8', errors='replace') for s in raw_strings if len(s) > 2]

print(f"Total C-strings extracted: {len(strings)}")

interesting_keywords = ['ftl', '.dat', 'ship', 'crew', 'weapon', 'drone', 'audio', 'sound', 'render', 'gl', 'save', 'profile', 'prof.sav', 'settings']
matches = {}

for kw in interesting_keywords:
    matches[kw] = [s for s in strings if kw in s.lower()][:8]

for kw, vals in matches.items():
    print(f"\n--- Keyword '{kw}' ({len(vals)} sample matches) ---")
    for v in vals:
        print(f"  {v}")
