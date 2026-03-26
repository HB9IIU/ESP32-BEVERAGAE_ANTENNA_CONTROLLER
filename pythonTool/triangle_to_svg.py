import math

def rot_rel(x, y, rad):
    s = math.sin(rad)
    c = math.cos(rad)
    return (x * c - y * s, x * s + y * c)

def compute_notched_triangle_points(orbit_angle_deg, radius, side):
    center_x = 200  # SVG center
    center_y = 200
    th = math.radians(orbit_angle_deg)
    cx = center_x + radius * math.cos(th)
    cy = center_y + radius * math.sin(th)
    tri_angle_deg = orbit_angle_deg + 90.0
    rad = math.radians(tri_angle_deg)
    h = side * 0.86602540378
    top_rel = (0.0, -(2.0/3.0)*h)
    left_rel = (-side/2.0, +(1.0/3.0)*h)
    right_rel = (+side/2.0, +(1.0/3.0)*h)
    notch_rel = (0.0, 0.0)
    top = tuple(map(lambda v: cx + v, rot_rel(*top_rel, rad)))
    left = tuple(map(lambda v: cx + v, rot_rel(*left_rel, rad)))
    right = tuple(map(lambda v: cx + v, rot_rel(*right_rel, rad)))
    notch = tuple(map(lambda v: cx + v, rot_rel(*notch_rel, rad)))
    # Correction for y
    top = (top[0], cy + rot_rel(*top_rel, rad)[1])
    left = (left[0], cy + rot_rel(*left_rel, rad)[1])
    right = (right[0], cy + rot_rel(*right_rel, rad)[1])
    notch = (notch[0], cy + rot_rel(*notch_rel, rad)[1])
    return top, left, right, notch

def svg_triangle(triangle_points, filename="triangle.svg"):
    top, left, right, notch = triangle_points
    svg = f'''<svg width="400" height="400" xmlns="http://www.w3.org/2000/svg">
  <polygon points="{int(top[0])},{int(top[1])} {int(left[0])},{int(left[1])} {int(notch[0])},{int(notch[1])}" fill="red" stroke="black" stroke-width="2"/>
  <polygon points="{int(top[0])},{int(top[1])} {int(notch[0])},{int(notch[1])} {int(right[0])},{int(right[1])}" fill="red" stroke="black" stroke-width="2"/>
</svg>'''
    with open(filename, "w") as f:
        f.write(svg)
    print(f"SVG written to {filename}")

if __name__ == "__main__":
    # Example usage
    points = compute_notched_triangle_points(orbit_angle_deg=0, radius=100, side=60)
    svg_triangle(points)
