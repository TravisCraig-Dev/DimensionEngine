# Generates the engine icon: a rounded indigo tile with a lit isometric cube (needs Pillow).
#   python scripts/make_engine_icon.py Engine/Resources/Dimension.ico [preview.png]
import sys
from PIL import Image, ImageDraw

S = 1024  # supersampled master, downsampled for each icon size
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))

# Rounded tile with a vertical gradient.
grad = Image.new('RGBA', (S, S))
top, bottom = (58, 72, 140), (24, 22, 58)
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / (S - 1)
    gd.line([(0, y), (S, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([24, 24, S - 24, S - 24], radius=210, fill=255)
img.paste(grad, (0, 0), mask)

# Isometric cube: top, left and right faces.
d = ImageDraw.Draw(img)
cx, cy, r = S / 2, S / 2 + 30, 300
h = r * 0.5
topF = [(cx, cy - r), (cx + r * 0.866, cy - h), (cx, cy), (cx - r * 0.866, cy - h)]
leftF = [(cx - r * 0.866, cy - h), (cx, cy), (cx, cy + r), (cx - r * 0.866, cy + h)]
rightF = [(cx + r * 0.866, cy - h), (cx + r * 0.866, cy + h), (cx, cy + r), (cx, cy)]
d.polygon(topF, fill=(150, 220, 255, 255))
d.polygon(leftF, fill=(70, 140, 235, 255))
d.polygon(rightF, fill=(120, 90, 230, 255))
# Edge highlights.
for poly in (topF, leftF, rightF):
    d.line(poly + [poly[0]], fill=(235, 245, 255, 255), width=14, joint='curve')

sizes = [16, 24, 32, 48, 64, 256]
frames = [img.resize((s, s), Image.LANCZOS) for s in sizes]
frames[-1].save(sys.argv[1], format='ICO', sizes=[(s, s) for s in sizes], append_images=frames[:-1])
if len(sys.argv) > 2:
    img.resize((256, 256), Image.LANCZOS).save(sys.argv[2])
print('ok')
