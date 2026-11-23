# Neutral app icon: a rounded dark tile with a stylized orange "U".
import sys
from PIL import Image, ImageDraw
out = sys.argv[1]
S = 1024
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((32, 32, S - 32, S - 32), radius=200, fill=(22, 26, 38, 255), outline=(255, 170, 60, 255), width=36)
# U: two bars + bottom arc
w = 150
L, R, T, B = 270, S - 270, 230, 700
d.rectangle((L, T, L + w, B), fill=(255, 176, 60, 255))
d.rectangle((R - w, T, R, B), fill=(255, 176, 60, 255))
d.pieslice((L, B - (R - L) // 2, R, B + (R - L) // 2), 0, 180, fill=(255, 176, 60, 255))
inner = (L + w, B - (R - L - 2 * w) // 2, R - w, B + (R - L - 2 * w) // 2)
d.pieslice(inner, 0, 180, fill=(22, 26, 38, 255))
d.rectangle((L + w, T, R - w, B), fill=(22, 26, 38, 255))
im = im.resize((256, 256), Image.LANCZOS)
im.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (256, 256)])
print("ok")
