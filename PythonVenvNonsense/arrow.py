from __future__ import annotations

import math
import os
from dataclasses import dataclass
from typing import List, Tuple, Optional

from PIL import Image, ImageDraw


Point = Tuple[float, float]


def polygon_centroid(points: List[Point]) -> Point:
    area = 0.0
    cx = 0.0
    cy = 0.0
    n = len(points)
    for i in range(n):
        x0, y0 = points[i]
        x1, y1 = points[(i + 1) % n]
        cross = x0 * y1 - x1 * y0
        area += cross
        cx += (x0 + x1) * cross
        cy += (y0 + y1) * cross
    area *= 0.5
    if abs(area) < 1e-9:
        return (sum(p[0] for p in points) / n, sum(p[1] for p in points) / n)
    cx /= (6.0 * area)
    cy /= (6.0 * area)
    return (cx, cy)


def rotate_rel(rel: List[Point], angle_deg: float) -> List[Point]:
    ang = math.radians(angle_deg)
    ca, sa = math.cos(ang), math.sin(ang)
    return [(x * ca - y * sa, x * sa + y * ca) for x, y in rel]


@dataclass
class Style:
    size: int = 480
    outline_px: int = 0
    safety_px: int = 0
    fill_rgba: Tuple[int, int, int, int] = (84, 181, 164, 255)
    outline_rgba: Tuple[int, int, int, int] = (255, 255, 255, 255)


class FastArrow:
    def __init__(self, style: Style):
        self.st = style

        # 5-point polygon with a tiny flat notch
        notch_y = 0.62
        notch_half_w = 0.02
        self.base_norm: List[Point] = [
            (0.50, 0.06),                      # top
            (0.94, 0.95),                      # BR
            (0.50 + notch_half_w, notch_y),    # notch right
            (0.50 - notch_half_w, notch_y),    # notch left
            (0.06, 0.95),                      # BL
        ]

        cx, cy = polygon_centroid(self.base_norm)
        self.rel_norm = [(x - cx, y - cy) for x, y in self.base_norm]
        self.r_norm = max(math.hypot(x, y) for x, y in self.rel_norm)

    def render_rgba(self, angle_deg: float, fill_rgba: Optional[Tuple[int,int,int,int]] = None) -> Image.Image:
        st = self.st
        fill = fill_rgba if fill_rgba is not None else st.fill_rgba

        S = st.size
        cx = cy = S / 2.0

        # Safe for 360° rotation
        r_avail = (S / 2.0) - (st.outline_px / 2.0) - st.safety_px
        if r_avail <= 5:
            raise ValueError("Canvas too small for outline/safety settings.")

        scale_px = r_avail / self.r_norm
        rel_rot = rotate_rel(self.rel_norm, angle_deg)
        pts = [(cx + x * scale_px, cy + y * scale_px) for x, y in rel_rot]

        img = Image.new("RGBA", (S, S), (0, 0, 0, 0))  # <- transparent background
        draw = ImageDraw.Draw(img, "RGBA")
        draw.polygon(pts, fill=fill, outline=st.outline_rgba, width=st.outline_px)
        return img

    def save_png(self, angle_deg: float, path: str):
        self.render_rgba(angle_deg).save(path, "PNG")

    def save_360_pngs(self, out_dir: str, prefix="arrow_", log_every: int = 30):
        os.makedirs(out_dir, exist_ok=True)
        print(f"[PNG] Exporting 360 frames -> {os.path.abspath(out_dir)}")
        for a in range(360):
            self.save_png(a, os.path.join(out_dir, f"{prefix}{a:03d}.png"))
            if a == 0 or (a + 1) % log_every == 0 or a == 359:
                print(f"[PNG] {a+1}/360 (angle={a}°)")
        print("[PNG] Done.")

    @staticmethod
    def rgba_to_gif_frame_keyed(img_rgba: Image.Image, alpha_threshold: int = 1) -> Tuple[Image.Image, int]:
        """
        Robust GIF transparency:
        - Composite RGBA onto a KEY background color (highly visible + guaranteed present).
        - Quantize.
        - Find the palette index of that KEY color.
        - Force transparent pixels to that index.
        """
        KEY = (0, 255, 0)  # bright green key color
        w, h = img_rgba.size

        # Alpha mask: where to be transparent
        alpha = img_rgba.getchannel("A")
        trans_mask = alpha.point(lambda a: 255 if a <= alpha_threshold else 0)

        # Composite onto key background (so key color dominates transparent area)
        bg = Image.new("RGBA", (w, h), (*KEY, 255))
        comp = Image.alpha_composite(bg, img_rgba).convert("RGB")

        # Quantize to palette
        p = comp.quantize(colors=256, method=Image.Quantize.MEDIANCUT)

        # Find KEY in palette
        pal = p.getpalette()
        key_index = None
        for i in range(256):
            r, g, b = pal[i*3:(i+1)*3]
            if (r, g, b) == KEY:
                key_index = i
                break
        if key_index is None:
            # Force it into index 0 as fallback
            key_index = 0
            pal[0:3] = list(KEY)
            p.putpalette(pal)

        # Force transparent pixels to key_index
        px = p.load()
        tm = trans_mask.load()
        for y in range(h):
            for x in range(w):
                if tm[x, y] != 0:
                    px[x, y] = key_index

        p.info["transparency"] = key_index
        p.info["disposal"] = 2
        return p, key_index

    def save_gif(
        self,
        out_path: str,
        duration_ms: int = 20,
        loop: int = 0,
        step_deg: int = 1,
        alpha_threshold: int = 1,
        log_every: int = 30
    ):
        angles = list(range(0, 360, step_deg))
        print(f"[GIF] Rendering {len(angles)} frames -> {os.path.abspath(out_path)}")

        frames: List[Image.Image] = []
        transparency_index = None

        for i, a in enumerate(angles, start=1):
            rgba = self.render_rgba(a)
            p, idx = self.rgba_to_gif_frame_keyed(rgba, alpha_threshold=alpha_threshold)
            frames.append(p)
            transparency_index = idx  # use last (same KEY, so should be consistent)

            if i == 1 or (i % log_every) == 0 or i == len(angles):
                print(f"[GIF] {i}/{len(angles)} (angle={a}°)")

        print("[GIF] Writing...")
        frames[0].save(
            out_path,
            save_all=True,
            append_images=frames[1:],
            duration=duration_ms,
            loop=loop,
            disposal=2,
            transparency=transparency_index,
            optimize=False,
        )
        print("[GIF] Done.")


def main():
    arrow = FastArrow(Style(size=512, outline_px=12, safety_px=60))

    # Single PNG (transparent alpha; may LOOK white in Chrome because page is white)
    #arrow.save_png(0, "north.png")
    #print("[MAIN] Saved:", os.path.abspath("north.png"))

    # 360 transparent PNGs
    arrow.save_360_pngs("frames", prefix="arrow_", log_every=30)

    # Transparent GIF (1-bit transparency)
    arrow.save_gif("arrow_spin.gif", duration_ms=20, step_deg=1, log_every=30)
    print("[MAIN] Saved:", os.path.abspath("arrow_spin.gif"))


if __name__ == "__main__":
    main()
