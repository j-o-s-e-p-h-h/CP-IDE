"""Builds src/res/cp-ide.ico from ui/assets/logo-white.png: the white cp monogram on a
transparent background (no tile). Run once when the logo changes:
    python tools/make_icon.py
"""
import os
from PIL import Image, ImageFilter

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
logo = Image.open(os.path.join(root, "ui", "assets", "logo-white.png")).convert("RGBA")

# The source monogram is only 63x74 px. Rebuild a smooth silhouette from its alpha
# (upscale, blur, sharpen the edge) and fill it with white.
alpha = logo.getchannel("A")
colour = (255, 255, 255)
BIG = 1024
w, h = alpha.size
k = min(BIG * 0.92 / w, BIG * 0.92 / h)
mask = alpha.resize((int(w * k), int(h * k)), Image.BICUBIC).filter(ImageFilter.GaussianBlur(6))
mask = mask.point(lambda a: max(0, min(255, (a - 128) * 6 + 128)))
mono = Image.new("RGBA", mask.size, colour + (0,))
mono.putalpha(mask)

frames = []
for size in (256, 128, 64, 48, 32, 16):
    img = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
    img.alpha_composite(mono, ((BIG - mono.size[0]) // 2, (BIG - mono.size[1]) // 2))
    frames.append(img.resize((size, size), Image.LANCZOS))

out = os.path.join(root, "src", "res", "cp-ide.ico")
os.makedirs(os.path.dirname(out), exist_ok=True)
frames[0].save(out, format="ICO", sizes=[f.size for f in frames], append_images=frames[1:])
frames[0].save(os.path.join(root, "ui", "assets", "icon-256.png"))
print("wrote", out)
