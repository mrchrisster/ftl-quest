import json

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)

all_symbols = sorted(list(set(d['__TEXT,__stubs'] + d['__DATA,__got'])))

print(f"Total distinct symbols needed: {len(all_symbols)}")

# Categorize
entries = []
for sym in all_symbols:
    if sym == "0x80000000": continue
    clean = sym.lstrip("_")
    entries.append(sym)

print(f"Non-null symbols to map: {len(entries)}")
