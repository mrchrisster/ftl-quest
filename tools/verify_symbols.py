import json

with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\indirect_symbols.json') as f:
    d = json.load(f)

stubs = set(d['__TEXT,__stubs'])
got = set(d['__DATA,__got'])

print(f"Verifying {len(stubs)} stubs and {len(got)} GOT entries...")
