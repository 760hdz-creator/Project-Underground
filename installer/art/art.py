# Neutral placeholder art for the Project Underground installer (no game art).
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter
out = sys.argv[1]
W, H = 1600, 893
random.seed(7)
# diagonal dark gradient: deep navy (top-left) -> charcoal-violet (bottom-right)
grad = Image.new("RGB", (W, H))
px = grad.load()
c0, c1 = (10, 14, 24), (34, 24, 40)
for y in range(H):
    for x in range(W):
        t = (x / W * 0.6 + y / H * 0.4)
        px[x, y] = tuple(int(c0[i] + (c1[i] - c0[i]) * t) for i in range(3))
# soft glows on the right (the panel sits on the left)
glow = Image.new("RGB", (W, H), (0, 0, 0))
g = ImageDraw.Draw(glow)
g.ellipse((1050, 120, 1550, 620), fill=(120, 70, 20))
g.ellipse((900, 500, 1300, 900), fill=(20, 60, 90))
glow = glow.filter(ImageFilter.GaussianBlur(160))
img = Image.blend(grad, Image.composite(glow, grad, Image.new("L", (W, H), 255)), 0.0)
img = Image.fromarray(__import__("numpy").clip(__import__("numpy").asarray(grad, dtype=int) + __import__("numpy").asarray(glow, dtype=int), 0, 255).astype("uint8")) if False else None
from PIL import ImageChops
img = ImageChops.add(grad, glow)
# abstract shapes: thin diagonal ramp-like lines and a few outlined arcs
lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(lay)
for i in range(14):
    x0 = 820 + i * 60
    d.line((x0, H, x0 + 420, 0), fill=(255, 180, 80, 10 + i * 2), width=2)
for r in (180, 260, 340):
    d.arc((1300 - r, 420 - r, 1300 + r, 420 + r), 200, 330, fill=(255, 190, 110, 40), width=3)
d.arc((1000, 560, 1500, 1060), 180, 270, fill=(120, 190, 255, 45), width=4)
lay = lay.filter(ImageFilter.GaussianBlur(1))
img = Image.alpha_composite(img.convert("RGBA"), lay).convert("RGB")
img.save(out, optimize=True)
print(img.size)
