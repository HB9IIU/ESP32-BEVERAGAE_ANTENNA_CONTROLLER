from pathlib import Path
from PIL import Image

# --- inputs / settings ---
INPUT_PNG = "plane.png"
OUT_DIR = Path("360")

THRESH = 180          # your main threshold (same as before)
FINAL_SIZE = (32, 32) # final mask size
RETHRESH = 128        # re-threshold after resizing (adjust if needed)

# --- load base image ---
img = Image.open(INPUT_PNG).convert("L")  # grayscale

# --- make output folder ---
OUT_DIR.mkdir(exist_ok=True)

for deg in range(360):
    # 1) rotate the grayscale image
    # fillcolor=255 keeps background white after rotation
    rot = img.rotate(
        deg,
        resample=Image.Resampling.BICUBIC,
        expand=True,
        fillcolor=255
    )

    # 2) threshold to 1-bit (rough mask at rotated size)
    bw = rot.point(lambda p: 255 if p > THRESH else 0, mode="1")

    # 3) resize to 32x32 (resize in grayscale, then force 1-bit again)
    g = bw.convert("L")
    g32 = g.resize(FINAL_SIZE, resample=Image.Resampling.LANCZOS)
    bw32 = g32.point(lambda p: 255 if p > RETHRESH else 0, mode="1")

    # 4) save PBM
    out_path = OUT_DIR / f"plane_{deg:03d}.pbm"
    bw32.save(out_path)

print(f"OK: wrote 360 masks to: {OUT_DIR.resolve()}")
