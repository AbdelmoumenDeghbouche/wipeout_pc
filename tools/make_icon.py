"""The launcher icon (launcher/gow3.png, gow3.ico): a white anti-gravity racer seen from behind on a blue
rounded square. Original art: no game logo."""
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
# background: rounded square, vertical gradient from deep navy to blue, thin light rim
grad = Image.new('RGBA', (S, S))
top, bottom = (8, 20, 48), (24, 78, 170)
for y in range(S):
    t = y / (S - 1)
    grad.paste(tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,), (0, y, S, y + 1))
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((40, 40, S - 40, S - 40), radius=190, fill=255)
img.paste(grad, (0, 0), mask)
d = ImageDraw.Draw(img)
d.rounded_rectangle((40, 40, S - 40, S - 40), radius=190, outline=(120, 180, 255, 255), width=14)
# speed lines
for x0, y0, x1, y1 in ((150, 330, 330, 330), (120, 420, 300, 420), (694, 330, 874, 330), (724, 420, 904, 420)):
    d.line((x0, y0, x1, y1), fill=(90, 200, 255, 200), width=18)
# engine glow under the ship
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
g = ImageDraw.Draw(glow)
for cx in (452, 572):
    g.ellipse((cx - 80, 720, cx + 80, 880), fill=(60, 220, 255, 230))
glow = glow.filter(ImageFilter.GaussianBlur(40))
img.alpha_composite(glow)
# the racer: a flat wedge hull with two side pods, seen from behind
hull = [(512, 210), (600, 560), (650, 700), (640, 770), (384, 770), (374, 700), (424, 560)]
pods = [[(420, 520), (236, 690), (226, 790), (300, 800), (392, 740)],
        [(604, 520), (788, 690), (798, 790), (724, 800), (632, 740)]]
shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
sd = ImageDraw.Draw(shadow)
for shape in [hull] + pods:
    sd.polygon([(x + 14, y + 18) for x, y in shape], fill=(0, 0, 0, 150))
img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(12)))
d = ImageDraw.Draw(img)
for shape in pods:
    d.polygon(shape, fill=(205, 220, 245, 255))
d.polygon(hull, fill=(245, 248, 255, 255))
d.line([(420, 520), (392, 740)], fill=(150, 175, 215, 255), width=8)
d.line([(604, 520), (632, 740)], fill=(150, 175, 215, 255), width=8)
# cockpit and the engine nozzles
d.polygon([(512, 400), (548, 600), (512, 650), (476, 600)], fill=(30, 90, 190, 255))
for cx in (452, 572):
    d.rounded_rectangle((cx - 40, 712, cx + 40, 796), radius=16, fill=(40, 60, 100, 255))
    d.ellipse((cx - 26, 738, cx + 26, 790), fill=(120, 235, 255, 255))
out_png, out_ico = sys.argv[1], sys.argv[2]
img.resize((128, 128), Image.LANCZOS).save(out_png)
img.resize((256, 256), Image.LANCZOS).save(out_ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((512, 512), Image.LANCZOS).save(out_png.replace('.png', '_preview.png'))
