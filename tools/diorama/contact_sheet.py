#!/usr/bin/env python3
"""Make a contact sheet of a plane capture dir: frame + plane0..4 over a checkerboard."""
import sys, os
from PIL import Image, ImageDraw
d = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "sheet.png")
names = ["latest.ppm"] + [f"plane{i}.png" for i in range(5)]
tiles = []
for n in names:
    p = os.path.join(d, n)
    if not os.path.exists(p): continue
    im = Image.open(p).convert("RGBA")
    im = im.resize((im.width // 2, im.height // 2))
    bg = Image.new("RGBA", im.size, (255, 0, 255, 255))
    ck = ImageDraw.Draw(bg)
    for y in range(0, im.height, 16):
        for x in range(0, im.width, 16):
            if (x // 16 + y // 16) % 2: ck.rectangle([x, y, x + 15, y + 15], fill=(200, 0, 200, 255))
    bg.alpha_composite(im)
    ImageDraw.Draw(bg).text((6, 6), n, fill=(255, 255, 0, 255))
    tiles.append(bg)
w = max(t.width for t in tiles); h = max(t.height for t in tiles)
sheet = Image.new("RGBA", (w * 3, h * 2), (0, 0, 0, 255))
for i, t in enumerate(tiles): sheet.paste(t, ((i % 3) * w, (i // 3) * h))
sheet.convert("RGB").save(out)
print(out)
