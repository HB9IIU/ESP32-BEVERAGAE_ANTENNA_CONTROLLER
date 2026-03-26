from pathlib import Path
from PIL import Image

# ----------------------------
# SETTINGS (adjust if needed)
# ----------------------------
INPUT_PNG = "plane.png"

OUT_PBM_DIR = Path("360")
OUT_HEADER  = Path("plane32_360.h")

NAME = "plane32"          # prefix inside the .h
FINAL_SIZE = (32, 32)     # output mask size

THRESH   = 180            # first threshold on rotated image (0..255)
RETHRESH = 128            # second threshold after resizing (0..255)

ROTATE_RESAMPLE = Image.Resampling.BICUBIC
RESIZE_RESAMPLE = Image.Resampling.LANCZOS

ROTATE_FILL = 255         # white background when rotating (important)

# ----------------------------
# PBM P4 writer (1-bit packed)
# ----------------------------
def pack_bits_msb_first(img_1bit: Image.Image) -> bytes:
    """
    Pack a mode '1' image into PBM P4 byte layout:
    - row-major
    - MSB first in each byte
    - each row padded to full bytes
    In PBM: 1 = black, 0 = white.
    We'll treat pixel 'on' (plane) as black (1).
    """
    if img_1bit.mode != "1":
        raise ValueError("pack_bits_msb_first expects a mode '1' image")

    w, h = img_1bit.size
    stride = (w + 7) // 8

    # Convert to 0/255 values; in mode '1' PIL typically uses 0 or 255.
    # We'll interpret 0 as black (1 in PBM), 255 as white (0 in PBM).
    pix = list(img_1bit.getdata())

    out = bytearray(stride * h)
    idx = 0
    for y in range(h):
        for xb in range(stride):
            byte = 0
            for bit in range(8):
                x = xb * 8 + bit
                if x >= w:
                    # padding bits are 0 (white)
                    continue
                p = pix[idx]
                idx += 1

                is_black = (p == 0)
                if is_black:
                    byte |= (1 << (7 - bit))  # MSB first
            out[y * stride + xb] = byte
        # If width not multiple of 8, the getdata() still advanced only w pixels,
        # which we already handled. No extra idx increments needed here.
    return bytes(out)

def write_pbm_p4(path: Path, img_1bit: Image.Image) -> None:
    w, h = img_1bit.size
    packed = pack_bits_msb_first(img_1bit)
    header = f"P4\n{w} {h}\n".encode("ascii")
    path.write_bytes(header + packed)

def read_pbm_p4(path: Path):
    """Return (w, h, pixel_bytes) for PBM P4 (binary)."""
    with path.open("rb") as f:
        magic = f.readline().strip()
        if magic != b"P4":
            raise ValueError(f"{path.name}: not PBM P4 (got {magic!r})")

        tokens = []
        while len(tokens) < 2:
            line = f.readline()
            if not line:
                raise ValueError(f"{path.name}: unexpected EOF in header")
            line = line.strip()
            if not line or line.startswith(b"#"):
                continue
            tokens += line.split()

        w = int(tokens[0])
        h = int(tokens[1])
        stride = (w + 7) // 8
        pix = f.read(stride * h)
        if len(pix) != stride * h:
            raise ValueError(f"{path.name}: pixel data size mismatch "
                             f"(expected {stride*h}, got {len(pix)})")
        return w, h, pix

# ----------------------------
# MAIN
# ----------------------------
def main():
    inp = Path(INPUT_PNG)
    if not inp.exists():
        raise SystemExit(f"ERROR: {INPUT_PNG} not found in this folder.")

    # Load as grayscale
    base = Image.open(inp).convert("L")

    # Make output directory
    OUT_PBM_DIR.mkdir(exist_ok=True)

    # 1) Generate 360 PBM masks
    print("Generating 360 PBM masks...")
    for deg in range(360):
        # Rotate (expand keeps full rotated bounds)
        rot = base.rotate(
            deg,
            resample=ROTATE_RESAMPLE,
            expand=True,
            fillcolor=ROTATE_FILL
        )

        # Threshold (produce a clean black/white image)
        # Here: bright -> white, dark -> black
        bw_rot = rot.point(lambda p: 255 if p > THRESH else 0, mode="L")

        # Resize down to 32x32
        small = bw_rot.resize(FINAL_SIZE, resample=RESIZE_RESAMPLE)

        # Re-threshold to force crisp binary again
        # Convert to mode '1' where 0=black, 255=white
        bw32 = small.point(lambda p: 255 if p > RETHRESH else 0, mode="1")

        # Write PBM P4
        out_path = OUT_PBM_DIR / f"plane_{deg:03d}.pbm"
        write_pbm_p4(out_path, bw32)

        if deg % 30 == 0:
            print(f"  ...{deg}/359")

    print(f"OK: PBMs written to {OUT_PBM_DIR.resolve()}")

    # 2) Build combined header from PBMs
    print("Building combined header...")

    widths = set()
    heights = set()
    blobs = []
    offsets = []
    total = 0

    for deg in range(360):
        pbm = OUT_PBM_DIR / f"plane_{deg:03d}.pbm"
        w, h, pix = read_pbm_p4(pbm)
        widths.add(w); heights.add(h)
        offsets.append(total)
        blobs.append(pix)
        total += len(pix)

    if len(widths) != 1 or len(heights) != 1:
        raise SystemExit(f"ERROR: inconsistent PBM sizes: widths={widths}, heights={heights}")

    w = next(iter(widths))
    h = next(iter(heights))
    stride = (w + 7) // 8
    bytes_per_mask = stride * h

    # offsets will be small for 32x32 (360*128 = 46080), so uint16_t is enough
    offset_type = "uint16_t" if max(offsets) <= 65535 else "uint32_t"

    with OUT_HEADER.open("w", encoding="utf-8") as out:
        out.write("#pragma once\n")
        out.write("#include <stdint.h>\n")
        out.write("#include <pgmspace.h>\n\n")

        out.write(f"// Generated by make_plane32_360.py\n")
        out.write(f"// Source: {INPUT_PNG}\n")
        out.write(f"// {NAME}: {w}x{h}, stride={stride} bytes/row, {bytes_per_mask} bytes/mask, 360 headings\n\n")

        out.write(f"static const uint16_t {NAME}_w = {w};\n")
        out.write(f"static const uint16_t {NAME}_h = {h};\n")
        out.write(f"static const uint16_t {NAME}_stride = {stride};\n")
        out.write(f"static const uint16_t {NAME}_bytes_per_mask = {bytes_per_mask};\n\n")

        out.write(f"static const {offset_type} {NAME}_offset[360] PROGMEM = {{\n")
        for i in range(0, 360, 12):
            chunk = offsets[i:i+12]
            out.write("  " + ", ".join(str(x) for x in chunk) + ",\n")
        out.write("};\n\n")

        out.write(f"static const uint8_t {NAME}_masks[{total}] PROGMEM = {{\n")
        for pix in blobs:
            for i in range(0, len(pix), 16):
                chunk = pix[i:i+16]
                out.write("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        out.write("};\n")

    print(f"OK: wrote {OUT_HEADER.resolve()}")
    print(f"    total_bytes={total} (360 * {bytes_per_mask} bytes)")

if __name__ == "__main__":
    main()
