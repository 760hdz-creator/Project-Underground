# App icon from the user's art (art/src/icon_source.jpg): cuts the rounded tile out of its dark backdrop.
# usage: python icon.py <out.ico> [<out.png 256x256>]
# The backdrop is a smooth, unsaturated dark grey (plus a soft drop shadow); it is flood-filled from the image
# border, and everything it does not reach is the tile.
import os, sys
from collections import deque
from PIL import Image, ImageFilter

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "icon_source.jpg")
out_ico = sys.argv[1]
out_png = sys.argv[2] if len(sys.argv) > 2 else None
im = Image.open(SRC).convert("RGB")
W, H = im.size
px = im.load()


def backdrop(p):
    r, g, b = p
    return max(r, g, b) - min(r, g, b) <= 14 and max(r, g, b) <= 72


seen = bytearray(W * H)
q = deque()
for x in range(W):
    q.append((x, 0)); q.append((x, H - 1))
for y in range(H):
    q.append((0, y)); q.append((W - 1, y))
while q:
    x, y = q.popleft()
    i = y * W + x
    if seen[i] or not backdrop(px[x, y]):
        continue
    seen[i] = 1
    if x > 0: q.append((x - 1, y))
    if x < W - 1: q.append((x + 1, y))
    if y > 0: q.append((x, y - 1))
    if y < H - 1: q.append((x, y + 1))
mask = Image.frombytes("L", (W, H), bytes(0 if s else 255 for s in seen)).filter(ImageFilter.MedianFilter(9))
# The fill also leaks into dark ink inside the tile. The tile is convex, so keep only its outline: a pixel is in when
# it lies between the outermost tile pixels of both its row and its column.
m = mask.load()
rows = [None] * H
cols = [None] * W
for y in range(H):
    xs = [x for x in range(W) if m[x, y]]
    if xs: rows[y] = (xs[0], xs[-1])
for x in range(W):
    ys = [y for y in range(H) if m[x, y]]
    if ys: cols[x] = (ys[0], ys[-1])
solid = bytearray(W * H)
for y in range(H):
    if rows[y]:
        for x in range(rows[y][0], rows[y][1] + 1):
            if cols[x] and cols[x][0] <= y <= cols[x][1]:
                solid[y * W + x] = 255
# soften the edge by a pixel
mask = Image.frombytes("L", (W, H), bytes(solid)).filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.2))
box = mask.point(lambda v: 255 if v > 128 else 0).getbbox()
tile = im.convert("RGBA")
tile.putalpha(mask)
tile = tile.crop(box)
# square canvas, tile centred, small margin
side = max(tile.size) * 102 // 100
sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
sq.paste(tile, ((side - tile.width) // 2, (side - tile.height) // 2), tile)
big = sq.resize((256, 256), Image.LANCZOS)
big.save(out_ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
if out_png:
    big.save(out_png)
print("icon", box, sq.size)
