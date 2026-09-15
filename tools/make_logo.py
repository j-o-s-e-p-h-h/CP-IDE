"""Rebuilds ui/assets/logo.png (the light-theme logo) from ui/assets/logo-white.png.
Run when the logo changes:  python tools/make_logo.py

The UI ships two copies of the monogram: white for the dark theme, dark for the
light one. Only the colour differs, so deriving one from the other keeps them from
drifting apart — the dark copy used to be a washed-out tan that all but vanished
against the light background.

The source is only 63x74, so it is rebuilt the way the icon is: upscale the alpha,
blur, hard-threshold. That gives a clean silhouette to fill and a file with enough
resolution for the 40px home header without the browser resampling a tiny bitmap.
"""
import os
from PIL import Image, ImageFilter

MARK = (28, 28, 30)   # --text of the light theme, so it matches the title beside it
SCALE = 4             # output is this many times the source

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "ui", "assets", "logo-white.png")
dst = os.path.join(root, "ui", "assets", "logo.png")

logo = Image.open(src).convert("RGBA")
alpha = logo.getchannel("A")
box = alpha.getbbox()
alpha = alpha.crop(box)

w, h = alpha.size
mask = alpha.resize((w * SCALE, h * SCALE), Image.LANCZOS)
mask = mask.filter(ImageFilter.GaussianBlur(SCALE / 2))
mask = mask.point(lambda a: max(0, min(255, (a - 128) * 8 + 128)))

out = Image.new("RGBA", mask.size, MARK + (0,))
out.putalpha(mask)
out.save(dst)
print(f"wrote {dst}  {out.size[0]}x{out.size[1]}  fill={MARK}")
