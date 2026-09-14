"""Builds src/res/cp-ide.ico and ui/assets/icon-256.png from ui/assets/logo-white.png.
Run when the logo changes:  python tools/make_icon.py

Two things make this more than a resize:

* The source monogram is only 63x74 px, so a plain downscale to 16 px turns the thin
  ring into grey mush. The silhouette is rebuilt from the alpha channel at high
  resolution (upscale, blur, hard threshold) and every icon size is rendered from
  that master with one LANCZOS step.
* A white mark on a bare transparent background disappears on a light Explorer
  background, so the mark carries a soft dark contour. It is invisible against dark
  (where the white already reads) and is what keeps the icon legible on white,
  without putting the monogram in a tile.

Windows picks whichever frame matches the current view and DPI; the sizes below are
the full set it asks for, so it never has to rescale one of ours itself.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

SIZES = (256, 128, 96, 64, 48, 40, 32, 24, 20, 16)
MARK = (255, 255, 255)         # the monogram
CONTOUR = (24, 24, 24)         # keyline behind it so white survives a light background
CONTOUR_ALPHA = 205            # 0 disables the keyline entirely
CONTOUR_WIDTH = 0.05           # keyline thickness as a fraction of the box (~1 px at 20 px)
MARK_FILL = 0.88               # how much of the box the monogram spans
BIG = 1024                     # master resolution everything is rendered from

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
logo = Image.open(os.path.join(root, "ui", "assets", "logo-white.png")).convert("RGBA")

# 1. Clean silhouette of the mark, cropped to its own ink so no padding is baked in.
alpha = logo.getchannel("A")
alpha = alpha.crop(alpha.getbbox())
w, h = alpha.size
span = int(BIG * MARK_FILL)
k = min(span / w, span / h)
mask = alpha.resize((max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS)
mask = mask.filter(ImageFilter.GaussianBlur(BIG / 220))
mask = mask.point(lambda a: max(0, min(255, (a - 128) * 8 + 128)))
mark = Image.new("RGBA", mask.size, MARK + (0,))
mark.putalpha(mask)

# 2. The master icon: the mark, over a soft dark contour spread from its own shape.
master = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
at = ((BIG - mark.size[0]) // 2, (BIG - mark.size[1]) // 2)
if CONTOUR_ALPHA > 0:
    # Grow the silhouette by blurring and re-thresholding: the spread has to be a
    # fraction of the icon, not a fixed pixel count, or it vanishes once scaled down.
    spread = BIG * CONTOUR_WIDTH
    halo = Image.new("L", (BIG, BIG), 0)
    halo.paste(mask, at)
    halo = halo.filter(ImageFilter.GaussianBlur(spread / 2))
    halo = halo.point(lambda v: 255 if v > 26 else int(v * 9))
    halo = halo.filter(ImageFilter.GaussianBlur(BIG / 400))
    halo = halo.point(lambda v: int(v / 255 * CONTOUR_ALPHA))
    shade = Image.new("RGBA", (BIG, BIG), CONTOUR + (0,))
    shade.putalpha(halo)
    master.alpha_composite(shade)
master.alpha_composite(mark, at)

# 3. One LANCZOS step per size. Below 32 px the antialiased edge of the tile turns
#    into a pale halo, so the alpha is pushed back towards solid.
frames = []
for size in SIZES:
    img = master.resize((size, size), Image.LANCZOS)
    if size <= 24:
        a = img.getchannel("A").point(lambda v: 0 if v < 40 else min(255, int(v * 1.35)))
        img.putalpha(a)
    frames.append(img)

out = os.path.join(root, "src", "res", "cp-ide.ico")
os.makedirs(os.path.dirname(out), exist_ok=True)
frames[0].save(out, format="ICO", sizes=[f.size for f in frames], append_images=frames[1:])
frames[0].save(os.path.join(root, "ui", "assets", "icon-256.png"))
print("wrote", out, "sizes:", [f.size[0] for f in frames])
