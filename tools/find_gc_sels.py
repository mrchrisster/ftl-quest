import struct

path = r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\inputs\FTL'
with open(path, 'rb') as f:
    f.seek(0x230000)
    data = f.read(2445152)

# Check selectors in __TEXT,__objc_methname that contain 'button' or 'dpad' or 'player' or 'controller'
with open(r'C:\Users\christoph\.gemini\antigravity\scratch\ftl-quest3\workspace\reports\objc_metadata.json') as f:
    import json
    objc = json.load(f)

gc_sels = [m for m in objc['methods'] if any(x in m.lower() for x in ['button', 'dpad', 'controller', 'player', 'gamepad', 'thumbstick', 'trigger'])]
print("GameController selectors found in binary:")
for s in gc_sels:
    print("  ", s)
