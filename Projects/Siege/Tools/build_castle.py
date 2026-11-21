"""Models/Castle/Castle.gltf: the castle on the crag's summit. A curtain wall with round towers follows the summit's
irregular rim (terrain_gen.rim_radius), a gatehouse faces east where the road arrives, a keep with corner turrets
stands at the back (north), and a hall, a chapel and timber houses fill the bailey.

The model's origin is the crag's center at height 0 (the scene places it at CASTLE_CENTER), so its y values are world
heights. Run after build_siege.py (it reads the terrain heights it caches) and then build_siege.py again to place it.
Also writes castle_layout.json (gate, wall walk, tower tops) for the battle script and the scene builder."""
import json
import math
import os

import numpy as np

import meshkit as K
import siege as S
import terrain_gen as T

H = T.CASTLE_HEIGHT
CX, CZ = T.CASTLE_CENTER
WALK = H + 12.5      # the curtain wall's wall walk
TOWER_TOP = H + 17.0

STONE = K.Material("Castle Stone", "Castle/castle_wall_slates", tile=3.0, color=(0.92, 0.9, 0.86, 1))
FOOTING = K.Material("Castle Footing", "Castle/mossy_stone_wall", tile=3.0)
TRIM = K.Material("Castle Trim", "Castle/defense_wall", tile=2.5, color=(0.85, 0.83, 0.8, 1))
WALKWAY = K.Material("Wall Walk", "Castle/cobblestone_floor_05", tile=3.0)
SLATE = K.Material("Slate Roof", "Castle/slate_tiles", tile=2.5)
TILES = K.Material("Tile Roof", "Castle/clay_roof_tiles_02", tile=2.5, color=(0.75, 0.62, 0.55, 1))
THATCH = K.Material("Thatch", "Castle/thatch_roof_angled", tile=3.0)
PLANKS = K.Material("Timber", "Castle/weathered_planks", tile=2.0)
PLASTER = K.Material("Plaster", "Castle/castle_wall_slates", tile=2.0, color=(1.0, 0.95, 0.85, 1))
WINDOW = K.Material("Window", None, color=(0.03, 0.025, 0.02, 1), roughness=0.9)

_heights = None


def ground(x, z):
    """Terrain height at local (x, z) (bilinear in the inner terrain's cached heights)."""
    global _heights
    if _heights is None:
        _heights = np.load(os.path.join(os.path.dirname(__file__), "cache_inner_heights.npy"))
    n = _heights.shape[0]
    size = 1400.0
    fx = np.clip(((x + CX) / size + 0.5) * (n - 1), 0, n - 1.001)
    fz = np.clip(((z + CZ) / size + 0.5) * (n - 1), 0, n - 1.001)
    c, r = int(fx), int(fz)
    tx, tz = fx - c, fz - r
    h = _heights
    return float((h[r, c] * (1 - tx) + h[r, c + 1] * tx) * (1 - tz) + (h[r + 1, c] * (1 - tx) + h[r + 1, c + 1] * tx) * tz)


def ground_min(points, margin=3.0):
    return min(ground(x, z) for x, z in points) - margin


def rim_point(angle_deg, inset):
    a = math.radians(angle_deg)
    r = float(T.rim_radius(np.array(a))) - inset
    return np.array([math.cos(a) * r, math.sin(a) * r])


# Towers around the rim (degrees; 0 = east, 90 = south toward the battlefield). The gate sits between the first two.
TOWERS = [-10, 10, 42, 74, 106, 138, 168, 198, 228, 258, 290, 322]
GATE_TOWERS = (0, 1)


def merlons_along(kit, a, b, y, outward, height=1.7, width=1.3, gap=1.1, depth=0.7):
    d = b - a
    length = np.linalg.norm(d)
    u = d / length
    yaw = math.atan2(-u[1], u[0])
    count = int((length - gap) // (width + gap))
    if count <= 0:
        return
    start = (length - (count * (width + gap) - gap)) / 2 + width / 2
    for i in range(count):
        p = a + u * (start + i * (width + gap)) + outward
        kit.box(TRIM, (p[0], y + height / 2, p[1]), (width, height, depth), yaw)


def curtain(kit, a, b, top=WALK):
    """A wall section between tower centers a and b: battered footing, wall, parapet with merlons, inner rail."""
    d = b - a
    u = d / np.linalg.norm(d)
    n = np.array([u[1], -u[0]])
    mid = (a + b) / 2
    if np.dot(n, mid) < 0:  # the outward side faces away from the castle's center
        n = -n
    samples = [a + d * t + n * s for t in np.linspace(0, 1, 9) for s in (-2.5, 2.5)]
    base = ground_min([(p[0], p[1]) for p in samples], 6.0)
    kit.segment_wall(FOOTING, a + n * 0.4, b + n * 0.4, base, H + 1.5, 4.4)
    kit.segment_wall(STONE, a, b, H + 1.4, top, 3.2, top_mat=WALKWAY)
    # parapet (outer) with merlons, and a low rail inside
    po = n * 1.25
    kit.segment_wall(STONE, a + po, b + po, top, top + 1.2, 0.7, top_mat=TRIM)
    merlons_along(kit, a, b, top + 1.2, po)
    pi = -n * 1.4
    kit.segment_wall(TRIM, a + pi, b + pi, top, top + 0.5, 0.4)
    return {"a": a.tolist(), "b": b.tolist(), "outward": n.tolist(), "walk": top}


def tower(kit, c, radius=5.0, top=TOWER_TOP, roof=False):
    pts = [(c[0] + math.cos(t) * (radius + 1), c[1] + math.sin(t) * (radius + 1)) for t in np.linspace(0, 2 * math.pi, 12)]
    base = ground_min(pts, 6.0)
    kit.cylinder(FOOTING, c, radius + 1.0, base, H + 1.0, 24, top_radius=radius)
    kit.cylinder(STONE, c, radius, H + 1.0, top - 0.8, 24)
    # arrow slits
    for k in range(6):
        t = k / 6 * 2 * math.pi + 0.3
        for y in (H + 6.0, H + 11.5):
            p = np.array([c[0] + math.cos(t) * (radius + 0.02), c[1] + math.sin(t) * (radius + 0.02)])
            tang = np.array([-math.sin(t), math.cos(t)]) * 0.22
            out = np.array([math.cos(t), 0, math.sin(t)])
            q = [np.array([p[0] - tang[0], y, p[1] - tang[1]]), np.array([p[0] + tang[0], y, p[1] + tang[1]]),
                 np.array([p[0] + tang[0], y + 1.6, p[1] + tang[1]]), np.array([p[0] - tang[0], y + 1.6, p[1] - tang[1]])]
            if np.dot(np.cross(q[1] - q[0], q[2] - q[0]), out) < 0:
                q = q[::-1]
            kit.poly(WINDOW, q)
    # corbelled machicolation ring, then the top
    kit.cylinder(TRIM, c, radius, top - 0.8, top, 24, top_radius=radius + 0.7)
    if roof:
        kit.cylinder(STONE, c, radius + 0.7, top, top + 1.4, 24, cap=True, cap_mat=WALKWAY)
        kit.cone(SLATE, c, radius + 1.4, top + 1.4, radius * 2.1, 24)
    else:
        kit.cylinder(STONE, c, radius + 0.7, top, top + 1.2, 24)
        kit.cylinder(STONE, c, radius, top, top + 1.2, 24, inside=True)
        kit.cylinder(WALKWAY, c, radius, top - 0.01, top, 24, cap=True, cap_mat=WALKWAY)
        count = int(2 * math.pi * (radius + 0.35) / 2.3)
        for k in range(count):
            t = (k + 0.5) / count * 2 * math.pi
            p = (c[0] + math.cos(t) * (radius + 0.35), c[1] + math.sin(t) * (radius + 0.35))
            kit.box(TRIM, (p[0], top + 1.2 + 0.8, p[1]), (1.2, 1.6, 0.7), -t + math.pi / 2)
    return {"center": [float(c[0]), float(c[1])], "radius": radius, "top": top + (1.4 if roof else 0.0), "roofed": roof}


def gatehouse(kit, a, b, top=H + 15.0, opening=(5.0, 7.0), depth=8.0):
    """The gate block between the gate towers: a passage through it, the walls above, merlons."""
    d = b - a
    length = np.linalg.norm(d)
    u = d / length
    n = np.array([u[1], -u[0]])
    mid = (a + b) / 2
    if np.dot(n, mid) < 0:
        n = -n
    ow, oh = opening
    side = (length - ow) / 2
    base = ground_min([(p[0], p[1]) for p in (a, b, mid + n * 4, mid - n * 4)], 6.0)
    gate_floor = H - 0.6
    for sgn in (-1, 1):
        c = mid + u * sgn * (ow / 2 + side / 2)
        kit.box(STONE, (c[0], (base + top) / 2, c[1]), (side, top - base, depth), math.atan2(-u[1], u[0]), top_mat=WALKWAY)
    c = mid
    kit.box(STONE, (c[0], (gate_floor + oh + top) / 2, c[1]), (ow, top - gate_floor - oh, depth), math.atan2(-u[1], u[0]),
            bottom=True, top_mat=WALKWAY)
    kit.box(FOOTING, (c[0], (base + gate_floor) / 2, c[1]), (ow, gate_floor - base, depth), math.atan2(-u[1], u[0]),
            top_mat=WALKWAY)
    # parapets on both faces
    for sgn in (1, -1):
        po = n * sgn * (depth / 2 - 0.35)
        kit.segment_wall(STONE, a + po, b + po, top, top + 1.2, 0.7, top_mat=TRIM)
        merlons_along(kit, a, b, top + 1.2, po)
    # where the door stands: in the passage, a little inside the outer face
    door = mid + n * (depth / 2 - 1.2)
    return {"door": [float(door[0]), gate_floor, float(door[1])], "yaw": float(math.atan2(n[0], n[1])),
            "opening": [ow, oh], "outward": n.tolist(), "walk": top}


def windows(kit, center, size, yaw, rows, per_side, height=1.8, width=0.8):
    """Dark window recesses on the four faces of a box-shaped building."""
    cx, cy, cz = center
    w, _, d = size
    cs, sn = math.cos(yaw), math.sin(yaw)
    ax, az = np.array([cs, 0, -sn]), np.array([sn, 0, cs])
    for face, (normal, along, half, span) in enumerate(((ax, az, w / 2, d), (-ax, -az, w / 2, d), (az, -ax, d / 2, w),
                                                         (-az, ax, d / 2, w))):
        for y in rows:
            for k in range(per_side):
                t = (k + 0.5) / per_side - 0.5
                p = np.array([cx, y, cz]) + normal * (half + 0.03) + along * t * span * 0.8
                q = [p - along * width / 2, p + along * width / 2, p + along * width / 2 + [0, height, 0],
                     p - along * width / 2 + [0, height, 0]]
                if np.dot(np.cross(q[1] - q[0], q[2] - q[0]), normal) < 0:
                    q = q[::-1]
                kit.poly(WINDOW, q)


def keep(kit, c, size=22.0, top=H + 31.0, yaw=0.18):
    half = size / 2
    cs, sn = math.cos(yaw), math.sin(yaw)
    ax, az = np.array([cs, -sn]), np.array([sn, cs])
    corners = [c + ax * sx * half + az * sz * half for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    base = ground_min([(p[0], p[1]) for p in corners], 4.0)
    kit.box(FOOTING, (c[0], (base + H + 1.5) / 2, c[1]), (size + 1.2, H + 1.5 - base, size + 1.2), yaw)
    kit.box(STONE, (c[0], (H + 1.5 + top) / 2, c[1]), (size, top - H - 1.5, size), yaw, top_mat=WALKWAY)
    windows(kit, (c[0], 0, c[1]), (size, 0, size), yaw, rows=(H + 9, H + 16, H + 23), per_side=3)
    # string course and parapet with merlons
    kit.box(TRIM, (c[0], top - 0.3, c[1]), (size + 0.8, 0.6, size + 0.8), yaw, top=False)
    for i in range(4):
        a, b = corners[i], corners[(i + 1) % 4]
        outward = (a + b) / 2 - c
        outward = outward / np.linalg.norm(outward) * 0.0
        kit.segment_wall(STONE, a, b, top, top + 1.3, 0.8, top_mat=TRIM)
        merlons_along(kit, a + (b - a) * 0.12, b - (b - a) * 0.12, top + 1.3, outward)
    # a turret on each corner, roofed
    turrets = []
    for p in corners:
        kit.cylinder(STONE, p, 3.2, H + 1.5, top + 6.0, 20)
        kit.cylinder(TRIM, p, 3.2, top + 5.4, top + 6.0, 20, top_radius=3.6)
        kit.cone(SLATE, p, 4.1, top + 6.0, 8.5, 20)
        turrets.append([float(p[0]), float(p[1])])
    return {"center": [float(c[0]), float(c[1])], "top": top, "turrets": turrets}


def hall(kit, c, w, d, wall_h, yaw, roof_rise, wall_mat=STONE, roof_mat=TILES, window_rows=None):
    base = ground(c[0], c[1]) - 2.5
    floor = H
    kit.box(wall_mat, (c[0], (base + floor + wall_h) / 2, c[1]), (w, floor + wall_h - base, d), yaw, top=False)
    if window_rows:
        windows(kit, (c[0], 0, c[1]), (w, 0, d), yaw, rows=window_rows, per_side=max(2, int(d // 5)))
    kit.gable_roof(roof_mat, (c[0], 0, c[1]), w, d, floor + wall_h, roof_rise, yaw, overhang=0.7, gable_mat=wall_mat)


def build():
    kit = K.Kit()
    layout = {"center": [float(CX), float(CZ)], "height": H, "walls": [], "towers": []}
    pts = [rim_point(a, 2.5) for a in TOWERS]
    # curtain walls between consecutive towers (except the gate gap)
    for i in range(len(pts)):
        j = (i + 1) % len(pts)
        if (i, j) == GATE_TOWERS:
            layout["gate"] = gatehouse(kit, pts[i], pts[j])
        else:
            layout["walls"].append(curtain(kit, pts[i], pts[j]))
    for i, p in enumerate(pts):
        gate = i in GATE_TOWERS
        layout["towers"].append(tower(kit, p, radius=6.0 if gate else 5.0, top=H + 20.0 if gate else TOWER_TOP,
                                      roof=(not gate) and i % 2 == 1))
    # the keep at the back (north, -z), toward the cliffs above the plains' far side
    layout["keep"] = keep(kit, np.array([-12.0, -22.0]))
    # the great hall along the south wall, a chapel to the west, timber houses by the gate
    hall(kit, np.array([10.0, 26.0]), 26, 11, 9, -0.12, 6.5, window_rows=(H + 3.5,))
    hall(kit, np.array([-34.0, 14.0]), 9, 18, 10, 0.25, 7.5, window_rows=(H + 4.5,))
    kit.cylinder(STONE, (-34.0 + math.sin(0.25) * 10, 14.0 + math.cos(0.25) * 10), 2.4, H - 2, H + 21, 16)
    kit.cone(SLATE, (-34.0 + math.sin(0.25) * 10, 14.0 + math.cos(0.25) * 10), 3.0, H + 21, 9, 16)
    for (x, z, yaw) in ((40, 22, 0.6), (44, -10, 1.4), (30, -30, 0.9), (22, 4, 1.7), (8, -44, 0.2)):
        hall(kit, np.array([float(x), float(z)]), 6.5, 9, 4.2, yaw, 3.6, wall_mat=PLANKS, roof_mat=THATCH)
    guid = kit.save("Models/Castle/Castle.gltf")
    layout["model"] = guid
    with open(os.path.join(os.path.dirname(__file__), "castle_layout.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(layout, f, indent=1)
    return layout


if __name__ == "__main__":
    build()
