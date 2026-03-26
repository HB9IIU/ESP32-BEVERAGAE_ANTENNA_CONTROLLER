#!/usr/bin/env python3
"""
gen_dialBg_raw.py  —  Crop greatcircleMap.png around the needle pivot and
                       save as raw big-endian RGB565 for LittleFS.

Output: data/dialBg.raw
  Layout: CACHE_W × CACHE_W pixels, row-major, big-endian uint16 (RGB565).
  Row N starts at byte offset  N × CACHE_W × 2.
  Pixel (col) in row N starts at byte offset (N × CACHE_W + col) × 2.

Run from the project root:
    python3 tools/gen_dialBg_raw.py
"""

import struct, sys
from pathlib import Path

# ── must match needleStuff.h ──────────────────────────────────────────────────
CX       = 240        # needle pivot X (screen coords)
CY       = 160        # needle pivot Y (screen coords)
NEEDLE_L = 140        # desired needle length
CACHE_R  = NEEDLE_L + 3          # 133
CACHE_W  = 2 * CACHE_R + 1       # 267
# ─────────────────────────────────────────────────────────────────────────────

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  pip install Pillow")

project_root = Path(__file__).resolve().parent.parent
src_png  = project_root / "data" / "greatcircleMap.png"
dst_raw  = project_root / "data" / "dialBg.raw"

if not src_png.exists():
    sys.exit(f"Source not found: {src_png}")

img = Image.open(src_png).convert("RGB")
W, H = img.size
print(f"Source image: {W}×{H}  ({src_png.name})")

# Crop region centred at (CX, CY)
left   = CX - CACHE_R
top    = CY - CACHE_R
right  = left + CACHE_W
bottom = top  + CACHE_W

# Clamp to image bounds (pad with black if needed)
if left < 0 or top < 0 or right > W or bottom > H:
    print(f"Warning: crop ({left},{top})–({right},{bottom}) extends outside "
          f"image ({W}×{H}); padding with black.")
    padded = Image.new("RGB", (CACHE_W, CACHE_W), (0, 0, 0))
    # source region that is inside the image
    sx = max(left, 0);  sy = max(top, 0)
    ex = min(right, W); ey = min(bottom, H)
    region = img.crop((sx, sy, ex, ey))
    padded.paste(region, (sx - left, sy - top))
    crop = padded
else:
    crop = img.crop((left, top, right, bottom))

assert crop.size == (CACHE_W, CACHE_W), f"unexpected crop size {crop.size}"

# Convert to big-endian RGB565
pixels = crop.load()
out = bytearray(CACHE_W * CACHE_W * 2)
idx = 0
for y in range(CACHE_W):
    for x in range(CACHE_W):
        r, g, b = pixels[x, y]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        struct.pack_into(">H", out, idx, rgb565)   # big-endian
        idx += 2

dst_raw.write_bytes(out)
print(f"Written: {dst_raw}  ({len(out)} bytes, {CACHE_W}×{CACHE_W} pixels)")
print(f"NEEDLE_L={NEEDLE_L}, CACHE_R={CACHE_R}, CACHE_W={CACHE_W}")
