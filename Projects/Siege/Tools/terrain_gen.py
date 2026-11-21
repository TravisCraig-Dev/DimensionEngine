"""The Siege landscape: one height function for the whole valley, sampled by two terrains (a detailed inner one around
the castle mountain and the battlefield, a coarse outer one for the ranges around), plus layer weights.

World layout (meters, +Z south toward the viewer's side of the plains, +X east):
  castle mountain  centered at (0, -420), its summit plateau (the castle) at CASTLE_HEIGHT
  battlefield      the plains around (0, 160)
  the road         up the gentle east ridge from the plains (320, 80) to the gate on the plateau's east side
"""
import base64

import numpy as np

CASTLE_CENTER = np.array([0.0, -430.0])
CASTLE_HEIGHT = 235.0
CASTLE_RADIUS = 78.0          # the flat summit
BATTLE_CENTER = np.array([0.0, 170.0])
PLAINS_HEIGHT = 22.0

# The road from the plains to the gate (x, z), its height interpolated along it.
ROAD = np.array([[400, 300], [330, 90], [290, -120], [215, -250], [250, -340], [160, -395], [110, -428], [78, -430]],
                dtype=np.float64)

_rng = np.random.default_rng(1234)
_perm = np.concatenate([_rng.permutation(256)] * 2)
_grad = np.array([[np.cos(a), np.sin(a)] for a in np.linspace(0, 2 * np.pi, 16, endpoint=False)])


def perlin(x, z):
    xi, zi = np.floor(x).astype(np.int64), np.floor(z).astype(np.int64)
    xf, zf = x - xi, z - zi
    u, v = xf * xf * xf * (xf * (xf * 6 - 15) + 10), zf * zf * zf * (zf * (zf * 6 - 15) + 10)
    xi &= 255
    zi &= 255

    def g(ix, iz, dx, dz):
        h = _perm[_perm[ix] + iz] & 15
        return _grad[h, 0] * dx + _grad[h, 1] * dz

    n00, n10 = g(xi, zi, xf, zf), g(xi + 1, zi, xf - 1, zf)
    n01, n11 = g(xi, zi + 1, xf, zf - 1), g(xi + 1, zi + 1, xf - 1, zf - 1)
    return (n00 + (n10 - n00) * u) + ((n01 + (n11 - n01) * u) - (n00 + (n10 - n00) * u)) * v


def fbm(x, z, scale, octaves=5, gain=0.5, lacunarity=2.0, offset=0.0):
    total, amp, freq = 0.0, 1.0, scale
    for o in range(octaves):
        total = total + perlin(x * freq + offset + o * 37.1, z * freq - offset + o * 11.7) * amp
        amp *= gain
        freq *= lacunarity
    return total


def ridged(x, z, scale, octaves=6, offset=0.0):
    total, amp, freq, weight = 0.0, 0.6, scale, 1.0
    for o in range(octaves):
        n = 1.0 - np.abs(perlin(x * freq + offset + o * 19.3, z * freq + o * 7.9))
        n = n * n * weight
        weight = np.clip(n * 2.0, 0, 1)
        total = total + n * amp
        amp *= 0.5
        freq *= 2.1
    return total


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def road_distance(x, z):
    """Distance to the road's polyline and how far along it (0-1) the nearest point is."""
    best = np.full(x.shape, np.inf)
    along = np.zeros(x.shape)
    seg_len = np.linalg.norm(np.diff(ROAD, axis=0), axis=1)
    total = seg_len.sum()
    start = 0.0
    for (ax, az), (bx, bz), length in zip(ROAD[:-1], ROAD[1:], seg_len):
        dx, dz = bx - ax, bz - az
        t = np.clip(((x - ax) * dx + (z - az) * dz) / (length * length), 0, 1)
        d = np.hypot(x - (ax + dx * t), z - (az + dz * t))
        closer = d < best
        best = np.where(closer, d, best)
        along = np.where(closer, (start + t * length) / total, along)
        start += length
    return best, along


def ranges(x, z):
    wx = x + fbm(x, z, 1 / 2600, 3, offset=51) * 700
    wz = z + fbm(x, z, 1 / 2600, 3, offset=77) * 700
    massif = np.clip(fbm(wx, wz, 1 / 3200, 3, offset=13) * 0.7 + 0.55, 0, 1)
    peaks = ridged(wx, wz, 1 / 1500, 5, offset=9)
    detail = ridged(wx, wz, 1 / 380, 3, offset=29)
    return 380 + massif * 620 + peaks * massif * 780 + detail * 35 * (0.3 + massif)


def rim_radius(angle):
    """The summit's edge (where the castle walls stand) at an angle around the crag: irregular, not a circle."""
    c, s = np.cos(angle), np.sin(angle)
    return CASTLE_RADIUS + perlin(c * 1.3 + 40.5, s * 1.3 + 17.2) * 16 + perlin(c * 3.1 + 7.7, s * 3.1 + 3.3) * 6


def base_height(x, z):
    """The valley before the road and erosion: plains, the castle mountain, the ranges around."""
    r = np.hypot(x, z)
    # Rolling plains, slightly higher toward the mountains.
    h = PLAINS_HEIGHT + fbm(x, z, 1 / 420, 4) * 7 + fbm(x, z, 1 / 70, 3, offset=5) * 1.2
    # The highlands behind the castle (north), joining the ranges.
    dx, dz = x - CASTLE_CENTER[0], z - CASTLE_CENTER[1]
    behind = smoothstep(100, -500, dz)
    h = h + behind * (35 + 70 * ridged(x, z, 1 / 320, 4, offset=3)) * smoothstep(1400, 700, np.abs(dx))
    # The ranges around the valley, highest to the north, with snow: broad massifs (warped, so the ridgelines bend)
    # carrying a few big ridged peaks, rather than a carpet of equal spikes.
    north = smoothstep(-200, -2200, z)
    ring = smoothstep(1100, 2900, r) * (0.55 + 0.45 * north)
    h = h + ring * ranges(x, z) + north * smoothstep(600, 1400, r) * 120 * ridged(x, z, 1 / 420, 4, offset=2)
    # The castle crag: a cone falling from the summit, at a slope that depends on the side (a sheer south face toward
    # the battlefield, a gentle east ridge for the road, steep north and west flanks), cut by ridges and gullies.
    # Distance from the crag's center, scaled so the irregular rim sits at CASTLE_RADIUS.
    dtrue = np.maximum(np.hypot(dx, dz), 1)
    dc = dtrue * CASTLE_RADIUS / rim_radius(np.arctan2(dz, dx))
    cz, cx = dz / dtrue, dx / dtrue
    fall = 0.75 + 0.55 * np.maximum(cz, 0) - 0.55 * np.maximum(cx, 0) * (1 - np.maximum(cz, 0))
    out = np.maximum(dc - CASTLE_RADIUS, 0)
    # A cliff band under the walls (steep for the first ~60 m), then scree and slopes easing into the plains.
    cliff_band = 1 - np.exp(-out / 50)
    cone = CASTLE_HEIGHT - 3 - (115 * cliff_band + out * 0.3) * fall - out ** 1.35 * 0.003
    cone = cone - ridged(x, z, 1 / 70, 5, offset=6) * np.minimum(out * 0.35, 26) + fbm(x, z, 1 / 120, 3, offset=8) * 10
    # Vertical ribs and gullies down the cliffs: noise stretched along the fall line (around the crag it varies fast,
    # down it slowly), strongest in the cliff band.
    ang = np.arctan2(dz, dx)
    ribs = ridged(ang * 160, dc * 0.22, 1 / 9, 4, offset=17)
    cone = cone - ribs * 16 * cliff_band * np.exp(-out / 160) * smoothstep(0, 12, out)
    near = smoothstep(1100, 600, dc)
    # Smooth maximum of the ground and the crag (no crease where they meet), and nothing near rises above the summit.
    soft = 18.0
    crag = np.logaddexp(h / soft, cone / soft) * soft
    h = h * (1 - near) + crag * near
    h = np.minimum(h, CASTLE_HEIGHT - 3 + out * 0.9 * (1 - near) + out * 0.05)
    # The summit plateau the castle stands on (flattened, with a little crown).
    plateau = CASTLE_HEIGHT + fbm(x, z, 1 / 40, 2, offset=4) * 1.2
    k = smoothstep(CASTLE_RADIUS + 3, CASTLE_RADIUS - 5, dc)
    h = h * (1 - k) + plateau * k
    return h


def road_profile(samples=400, max_grade=0.2):
    """Heights along the road: the ground under it smoothed, never steeper than `max_grade`, ending at the gate."""
    seg = np.linalg.norm(np.diff(ROAD, axis=0), axis=1)
    cum = np.concatenate([[0], np.cumsum(seg)])
    s = np.linspace(0, cum[-1], samples)
    px = np.interp(s, cum, ROAD[:, 0])
    pz = np.interp(s, cum, ROAD[:, 1])
    g = base_height(px, pz)
    kernel = np.ones(25) / 25
    g = np.convolve(np.pad(g, 12, mode="edge"), kernel, mode="valid")
    g[-1] = CASTLE_HEIGHT - 1
    step = cum[-1] / (samples - 1) * max_grade
    for i in range(samples - 2, -1, -1):  # from the gate down: never steeper than the grade
        g[i] = max(min(g[i], g[i + 1] + step), g[i + 1] - step)
    return s / cum[-1], g


def carve_road(x, z, h):
    d, along = road_distance(x, z)
    t, profile = road_profile()
    road_h = np.interp(along, t, profile)
    # Below the ground: a narrow cutting. Above it: the ground rises in a broad ridge under the road.
    cut = smoothstep(22, 6, d)
    fill = smoothstep(190 + fbm(x, z, 1 / 80, 3, offset=12) * 60, 8, d) ** 2.2
    k = np.where(road_h > h, fill, cut)
    return h * (1 - k) + road_h * k, d


def thermal_erosion(h, spacing, iterations=40, talus_deg=38.0, rate=0.25, border=24, keep=None):
    """Material slides from slopes steeper than the talus angle onto the neighbours below (scree). Nothing moves
    within `border` samples of the edges, so the edges keep matching the surrounding terrain."""
    talus = np.tan(np.radians(talus_deg)) * spacing
    h = h.copy()
    n = h.shape[0]
    idx = np.arange(n)
    edge = np.minimum(np.minimum(idx, n - 1 - idx)[:, None], np.minimum(idx, n - 1 - idx)[None, :])
    active = np.clip((edge - border) / border, 0, 1)
    if keep is not None:
        active = active * (1 - keep)
    for _ in range(iterations):
        p = np.pad(h, 1, mode="edge")
        moved = np.zeros_like(h)
        for dy, dx in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            nb = p[1 + dy:1 + dy + n, 1 + dx:1 + dx + n]
            out = np.where(h - nb - talus > 0, (h - nb - talus) * rate * 0.25, 0) * active
            moved -= out
            # what leaves toward (dy, dx) arrives at that neighbour
            arrive = np.zeros_like(h)
            ys = slice(max(dy, 0), n + min(dy, 0))
            xs = slice(max(dx, 0), n + min(dx, 0))
            ys0 = slice(max(-dy, 0), n + min(-dy, 0))
            xs0 = slice(max(-dx, 0), n + min(-dx, 0))
            arrive[ys, xs] = out[ys0, xs0]
            moved += arrive
        h += moved
    return h


def grid(size, n):
    c = (np.arange(n) / (n - 1) - 0.5) * size
    return np.meshgrid(c, c)  # x (columns), z (rows)


def heights(size, n, erode=True):
    x, z = grid(size, n)
    h = base_height(x, z)
    if erode:
        # The crag's cliffs stay cliffs (erosion would slump them all to the talus angle).
        dc = np.hypot(x - CASTLE_CENTER[0], z - CASTLE_CENTER[1])
        h = thermal_erosion(h, size / (n - 1), keep=smoothstep(330, 200, dc))
    h, road = carve_road(x, z, h)
    return x, z, h, road


def slope_degrees(h, spacing):
    gz, gx = np.gradient(h, spacing)
    return np.degrees(np.arctan(np.hypot(gx, gz)))


def encode_heights(h, height_max):
    v = np.clip(np.round(h / height_max * 65535), 0, 65535).astype("<u2")
    return base64.b64encode(v.tobytes()).decode("ascii")


def encode_layers(weights):
    """weights: (layers, n, n) floats -> a byte per layer per sample (row-major), base64."""
    w = np.clip(weights, 0, None)
    w = w / np.maximum(w.sum(axis=0, keepdims=True), 1e-6)
    b = np.round(w * 255).astype(np.uint8)
    return base64.b64encode(np.ascontiguousarray(b.reshape(len(w), -1).T).tobytes()).decode("ascii")
