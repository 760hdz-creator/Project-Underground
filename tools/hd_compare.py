"""HD texture pack comparison images (README), Project Gauntlet style: original and HD full views side by side, a
yellow box on each, and the boxed area at 1:1 below.

Input: UG_SHOTAB=1 captures (shot_<N>.ppm with HD, shot_<N+1>.ppm without), widescreen (16:9 anamorphic) frames.
usage: python tools/hd_compare.py <capture dir> <out.jpg> [title]   (picks the best frame pair and box)
       python tools/hd_compare.py <capture dir> <out.jpg> --frame N [--box x,y]
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFile, ImageFilter, ImageFont

ImageFile.LOAD_TRUNCATED_IMAGES = True
CW = 790                      # each full view / zoom panel width
BOX_W, BOX_H = 790, 460       # 1:1 zoom box in presented pixels


def load(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return im.resize((round(h * 16 / 9), h), Image.LANCZOS)   # anamorphic 4:3 frame -> 16:9 as presented


def pairs(d):
    out = []
    for f in sorted(glob.glob(os.path.join(d, "shot_*.ppm"))):
        n = int(os.path.basename(f)[5:10])
        g = os.path.join(d, f"shot_{n + 1:05d}.ppm")
        if n % 100 == 0 and os.path.exists(g):
            out.append((n, f, g))
    return out


def motion_score(a, b):
    """Camera motion shows as large differences on edges; texture swaps as moderate ones on surfaces."""
    da = np.abs(np.asarray(a, np.int16) - np.asarray(b, np.int16)).sum(2)
    return float((da > 120).mean())


def detail(im):
    g = np.asarray(im.convert("L"), np.float32)
    lap = np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])
    return np.pad(lap, 1)


def best_box(a, b):
    """The box where the HD side gains the most fine detail (edge energy) over the original."""
    d = np.clip(detail(b) - detail(a), 0, None)
    h, w = d.shape
    ii = d.cumsum(0).cumsum(1)
    best, bx = -1.0, (0, 0)
    for y in range(0, h - BOX_H, 24):
        for x in range(0, w - BOX_W, 24):
            if y < h * 0.16 and x < w * 0.3:   # HUD (score) corner
                continue
            s = ii[y + BOX_H - 1, x + BOX_W - 1] - ii[y - 1 if y else 0, x + BOX_W - 1] * (y > 0) \
                - ii[y + BOX_H - 1, x - 1 if x else 0] * (x > 0) + (ii[y - 1, x - 1] if x and y else 0)
            if s > best:
                best, bx = s, (x, y)
    return bx


def compose(orig, hd, box, out, title=""):
    w, h = orig.size
    vh = round(CW * h / w)
    top, gap = 30, 34
    H = top + vh + gap + BOX_H + 10
    c = Image.new("RGB", (2 * CW + 20, H), (14, 18, 16))
    dr = ImageDraw.Draw(c)
    try:
        f = ImageFont.truetype("segoeuib.ttf", 20)
        fs = ImageFont.truetype("segoeui.ttf", 16)
    except OSError:
        f = fs = ImageFont.load_default()
    x0, y0 = box
    sx = CW / w
    for k, (im, label, col) in enumerate(((orig, "Original textures", (240, 240, 240)),
                                          (hd, "HD texture pack", (120, 230, 60)))):
        X = k * (CW + 20)
        c.paste(im.resize((CW, vh), Image.LANCZOS), (X, top))
        dr.text((X + 4, 4), label + (f"  ({title})" if title and k == 0 else ""), fill=col, font=f)
        dr.rectangle((X + x0 * sx, top + y0 * sx, X + (x0 + BOX_W) * sx, top + (y0 + BOX_H) * sx), outline=(230, 190, 40), width=2)
        c.paste(im.crop((x0, y0, x0 + BOX_W, y0 + BOX_H)), (X, top + vh + gap))
    dr.text((4, top + vh + 8), f"Zoom (1:1 at {w}x{h})", fill=(230, 190, 40), font=fs)
    c.save(out, quality=90, optimize=True, progressive=True)


def main():
    d, out = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    title = args[0] if args and not args[0].startswith("--") else ""
    frame = int(args[args.index("--frame") + 1]) if "--frame" in args else None
    box = tuple(int(v) for v in args[args.index("--box") + 1].split(",")) if "--box" in args else None
    cands = []
    for n, f_hd, f_or in pairs(d):
        if frame is not None and n != frame:
            continue
        if frame is None and n < 6000:   # menus / loading: skip
            continue
        a, b = load(f_or), load(f_hd)
        cands.append((motion_score(a, b), n, a, b))
    if not cands:
        sys.exit("no frame pairs")
    cands.sort(key=lambda t: t[0])
    m, n, a, b = cands[0]
    bx = box or best_box(a, b)
    compose(a, b, bx, out, title)
    print(f"{out}: frame {n}, motion {m:.4f}, box {bx}")


if __name__ == "__main__":
    main()
