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
MARK = (0, 0, 0)               # the monogram
CONTOUR = (255, 255, 255)      # keyline behind it so black survives a dark background
CONTOUR_ALPHA = 205            # 0 disables the keyline entirely
CONTOUR_WIDTH = 0.05           # keyline thickness as a fraction of the box
MIN_KEYLINE_PX = 2.0           # ...but never thinner than this in the finished frame
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

# 2. One master per output size. The keyline is what keeps a white mark readable on
#    a light background, and a single master shared by every size cannot provide it:
#    a 5% spread is 12 px at 256 but 0.8 px at 16, so it washes out in the downscale
#    and the small frames collapse into a dark smudge on white. Each size therefore
#    gets its own render, with the spread widened as needed so the finished keyline
#    is never thinner than MIN_KEYLINE_PX.
at = ((BIG - mark.size[0]) // 2, (BIG - mark.size[1]) // 2)


def master_for(size):
    scale = BIG / size                       # master pixels per finished pixel
    spread = max(BIG * CONTOUR_WIDTH, MIN_KEYLINE_PX * scale)
    img = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
    if CONTOUR_ALPHA > 0:
        halo = Image.new("L", (BIG, BIG), 0)
        halo.paste(mask, at)
        halo = halo.filter(ImageFilter.GaussianBlur(spread / 2))
        halo = halo.point(lambda v: 255 if v > 26 else int(v * 9))
        halo = halo.filter(ImageFilter.GaussianBlur(BIG / 400))
        halo = halo.point(lambda v: int(v / 255 * CONTOUR_ALPHA))
        shade = Image.new("RGBA", (BIG, BIG), CONTOUR + (0,))
        shade.putalpha(halo)
        img.alpha_composite(shade)
    img.alpha_composite(mark, at)
    return img


# 3. One LANCZOS step per size. Below 32 px the antialiased edge turns into a pale
#    halo, so the alpha is pushed back towards solid.
frames = []
for size in SIZES:
    img = master_for(size).resize((size, size), Image.LANCZOS)
    if size <= 24:
        a = img.getchannel("A").point(lambda v: 0 if v < 40 else min(255, int(v * 1.35)))
        img.putalpha(a)
    frames.append(img)

out = os.path.join(root, "src", "res", "cp-ide.ico")
os.makedirs(os.path.dirname(out), exist_ok=True)
frames[0].save(out, format="ICO", sizes=[f.size for f in frames], append_images=frames[1:])
frames[0].save(os.path.join(root, "ui", "assets", "icon-256.png"))
print("wrote", out, "sizes:", [f.size[0] for f in frames])
