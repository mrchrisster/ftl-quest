#!/usr/bin/env python3
"""Original logo for the FTL Quest Diorama port: stacked diorama panes with a ship.
Writes icon_1024.png (launcher icon master) and banner.png (README header)."""
import math, random, sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else "."
S = 1024
rnd = random.Random(7)

def space_bg(w, h):
    bg = Image.new("RGB", (w, h))
    px = bg.load()
    for y in range(h):
        for x in range(w):
            t = y / h
            d = math.hypot(x - w * 0.62, y - h * 0.38) / max(w, h)
            r = int(8 + 30 * max(0, 0.6 - d))
            g = int(10 + 22 * max(0, 0.6 - d) + 6 * (1 - t))
            b = int(26 + 70 * max(0, 0.7 - d) + 20 * (1 - t))
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(bg)
    for _ in range(int(w * h / 2600)):
        x, y = rnd.randrange(w), rnd.randrange(h)
        c = rnd.randint(120, 255)
        rr = rnd.choice([1, 1, 1, 2])
        d.ellipse([x, y, x + rr, y + rr], fill=(c, c, min(255, c + 20)))
    return bg

def pane(draw, cx, cy, w, h, skew, fill, outline, width=6):
    pts = [(cx - w / 2 + skew, cy - h / 2), (cx + w / 2 + skew, cy - h / 2),
           (cx + w / 2 - skew, cy + h / 2), (cx - w / 2 - skew, cy + h / 2)]
    draw.polygon(pts, fill=fill)
    draw.line(pts + [pts[0]], fill=outline, width=width, joint="curve")

def ship(draw, cx, cy, s):
    # original wedge cruiser silhouette, pointing right
    hull = [(cx - 1.0 * s, cy - 0.30 * s), (cx + 0.25 * s, cy - 0.38 * s), (cx + 1.0 * s, cy),
            (cx + 0.25 * s, cy + 0.38 * s), (cx - 1.0 * s, cy + 0.30 * s), (cx - 0.82 * s, cy)]
    draw.polygon(hull, fill=(214, 218, 224))
    draw.line(hull + [hull[0]], fill=(90, 100, 115), width=max(2, int(s * 0.04)))
    for k in (-1, 1):  # engine nacelles
        y = cy + k * 0.40 * s
        draw.rounded_rectangle([cx - 1.05 * s, y - 0.11 * s, cx - 0.35 * s, y + 0.11 * s], radius=0.08 * s,
                               fill=(190, 196, 204), outline=(90, 100, 115), width=max(2, int(s * 0.03)))
        draw.ellipse([cx - 1.30 * s, y - 0.09 * s, cx - 1.02 * s, y + 0.09 * s], fill=(255, 190, 80))
    draw.rectangle([cx - 0.55 * s, cy - 0.05 * s, cx + 0.45 * s, cy + 0.05 * s], fill=(230, 120, 70))  # stripe
    for i in range(3):  # cockpit / room windows
        x = cx + (0.05 + 0.18 * i) * s
        draw.rectangle([x - 0.05 * s, cy - 0.20 * s, x + 0.05 * s, cy - 0.12 * s], fill=(110, 220, 255))

def icon(size=S):
    img = space_bg(size, size).convert("RGBA")
    W, H, sk = size * 0.58, size * 0.24, size * 0.10
    def layer(fn):
        nonlocal img
        L = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        fn(ImageDraw.Draw(L))
        img = Image.alpha_composite(img, L.filter(ImageFilter.GaussianBlur(size * 0.010)))
        img = Image.alpha_composite(img, L)
    # back pane: space with a planet
    def back(d):
        pane(d, size * 0.46, size * 0.74, W, H, sk, (18, 28, 70, 235), (90, 140, 255, 255))
        d.ellipse([size * 0.26, size * 0.68, size * 0.38, size * 0.80], fill=(205, 110, 60, 255))
        d.ellipse([size * 0.52, size * 0.70, size * 0.545, size * 0.725], fill=(255, 240, 200, 255))
    # middle pane: the ship
    def mid(d):
        pane(d, size * 0.50, size * 0.52, W, H, sk, (40, 70, 130, 120), (120, 200, 255, 255))
        ship(d, size * 0.53, size * 0.52, size * 0.17)
    # front pane: interface frame with power bars
    def front(d):
        pane(d, size * 0.54, size * 0.29, W, H, sk, (30, 60, 50, 60), (120, 255, 190, 255), width=8)
        for i in range(4):
            x = size * (0.36 + 0.055 * i)
            d.rectangle([x, size * 0.335, x + size * 0.035, size * 0.355], fill=(90, 230, 110, 255))
        d.rounded_rectangle([size * 0.62, size * 0.32, size * 0.74, size * 0.37], radius=size * 0.01,
                            outline=(235, 245, 235, 255), width=5)
    layer(back); layer(mid); layer(front)
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=size * 0.18, fill=255)
    img.putalpha(m)
    return img

def banner(ic):
    w, h = 1600, 480
    b = space_bg(w, h).convert("RGBA")
    i = ic.resize((400, 400), Image.LANCZOS)
    b.alpha_composite(i, (60, 40))
    d = ImageDraw.Draw(b)
    try:
        f1 = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 92)
        f2 = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 44)
    except Exception:
        f1 = f2 = ImageFont.load_default()
    d.text((520, 140), "FTL Diorama", font=f1, fill=(235, 242, 255))
    d.text((524, 260), "Faster Than Light in VR on Meta Quest", font=f2, fill=(150, 205, 255))
    d.text((524, 320), "five depth layers · laser controls · native iOS port", font=f2, fill=(120, 255, 190))
    return b.convert("RGB")

os.makedirs(out, exist_ok=True)
ic = icon()
ic.save(os.path.join(out, "icon_1024.png"))
banner(ic).save(os.path.join(out, "banner.png"))
print("ok")
