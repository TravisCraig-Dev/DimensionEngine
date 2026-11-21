"""The cavalry's horses: Models/Horse/Horse_<Side>_<Coat>.gltf from Quaternius' low-poly horse (Models/Horse/Horse.gltf,
its skeleton and clips kept). The faceted mesh is welded and Loop-subdivided (skin weights carried along) into a smooth
body; the coat is a colour per variant over a pattern baked into the vertex colours (the source's darker and lighter
parts, black points on the lower legs and muzzle, a lighter belly, faint dapples) with a fine hair grain texture; and a
war saddle on a saddle cloth in the side's colours rides on top (shells of the body, skinned as it is).

Run: python build_horse.py (writes the models, their metas and Textures/Horse/coat_grain.png)."""
import copy
import os

import numpy as np
from PIL import Image

import gltfio as G
import siege as S

SRC = os.path.join(S.ASSETS, "Models", "Horse", "Horse.gltf")
OUT = os.path.join(S.ASSETS, "Models", "Horse")
GRAIN_REL = "Textures/Horse/coat_grain.png"

def srgb(r, g, b):
    """A colour picked in sRGB (0-255), as the linear values materials take."""
    c = np.array([r, g, b]) / 255.0
    return list(np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)) + [1.0]


TONE_MAX = 1.5  # the coat pattern's brightest (vertex colours hold up to 1: the pattern is stored divided by this)
COATS = {
    "Bay": srgb(96, 52, 28),
    "Chestnut": srgb(128, 62, 30),
    "Black": srgb(32, 28, 26),
    "Grey": srgb(150, 148, 144),
}
SIDES = {  # the saddle cloth and its trim (the soldiers' colours)
    "Defenders": {"cloth": srgb(30, 52, 120), "trim": srgb(196, 160, 70)},
    "Attackers": {"cloth": srgb(128, 22, 20), "trim": srgb(34, 28, 24)},
}
COAT_PARTS = {"Main": 1.0, "Main_Dark": 0.55, "Main_Light": 1.45, "Muzzle": 0.3}


# ---- mesh helpers -----------------------------------------------------------------------------------------------------

def weld(pos, *attrs, tol=1e-4):
    """Merges vertices at the same place (the source is flat shaded: every face has its own); returns the kept vertices'
    positions, each attribute averaged over the merged ones, and the old-to-new index map."""
    key = np.round(pos / tol).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    n = len(first)
    out = [pos[first]]
    counts = np.bincount(inverse, minlength=n).astype(np.float64)[:, None]
    for a in attrs:
        acc = np.zeros((n, a.shape[1]))
        np.add.at(acc, inverse, a)
        out.append(acc / counts)
    return out, inverse


def loop_subdivide(pos, tri, attrs):
    """One step of Loop subdivision: each triangle into four, the surface smoothed (boundaries kept). `attrs` (per
    vertex) are carried along linearly."""
    n = len(pos)
    edges = np.sort(np.vstack([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]]), axis=1)
    uniq, edge_of = np.unique(edges, axis=0, return_inverse=True)
    edge_of = edge_of.reshape(3, -1).T  # per triangle: its edges (01, 12, 20)
    m = len(tri)
    # The vertex opposite each edge in each of its triangles.
    opposite = np.vstack([tri[:, 2], tri[:, 0], tri[:, 1]]).T
    face_count = np.bincount(edge_of.reshape(-1), minlength=len(uniq))
    opp_sum = np.zeros((len(uniq), 3))
    np.add.at(opp_sum, edge_of.reshape(-1), pos[opposite.reshape(-1)])
    a, b = pos[uniq[:, 0]], pos[uniq[:, 1]]
    interior = face_count == 2
    edge_pos = np.where(interior[:, None], 0.375 * (a + b) + 0.125 * opp_sum, 0.5 * (a + b))
    # Old vertices: toward the average of their neighbours (Warren's weights); boundary ones stay.
    neighbours = np.zeros((n, 3))
    valence = np.zeros(n)
    np.add.at(neighbours, uniq[:, 0], pos[uniq[:, 1]])
    np.add.at(neighbours, uniq[:, 1], pos[uniq[:, 0]])
    np.add.at(valence, uniq[:, 0], 1)
    np.add.at(valence, uniq[:, 1], 1)
    on_boundary = np.zeros(n, bool)
    on_boundary[uniq[~interior].reshape(-1)] = True
    beta = np.where(valence > 3, 3.0 / (8.0 * np.maximum(valence, 1)), 3.0 / 16.0)
    smooth_pos = (1 - valence * beta)[:, None] * pos + beta[:, None] * neighbours
    new_old = np.where(on_boundary[:, None], pos, smooth_pos)
    out_pos = np.vstack([new_old, edge_pos])
    out_attrs = [np.vstack([at, 0.5 * (at[uniq[:, 0]] + at[uniq[:, 1]])]) for at in attrs]
    e = edge_of + n
    v0, v1, v2 = tri[:, 0], tri[:, 1], tri[:, 2]
    e01, e12, e20 = e[:, 0], e[:, 1], e[:, 2]
    out_tri = np.vstack([np.column_stack([v0, e01, e20]), np.column_stack([v1, e12, e01]),
                         np.column_stack([v2, e20, e12]), np.column_stack([e01, e12, e20])])
    del m
    return out_pos, out_tri, out_attrs


def dense_weights(joints, weights, count):
    d = np.zeros((len(joints), count))
    for k in range(4):
        np.add.at(d, (np.arange(len(joints)), joints[:, k].astype(int)), weights[:, k])
    return d


def top4(dense):
    idx = np.argsort(-dense, axis=1)[:, :4]
    w = np.take_along_axis(dense, idx, axis=1)
    w /= np.maximum(w.sum(axis=1, keepdims=True), 1e-9)
    return idx.astype(np.uint16), w.astype(np.float32)


def noise3(p, scale, seed):
    """Smooth value noise (a sum of a few random plane waves: cheap, tile-free)."""
    rng = np.random.default_rng(seed)
    out = np.zeros(len(p))
    for _ in range(6):
        d = rng.normal(size=3)
        d /= np.linalg.norm(d)
        out += np.sin(p @ d * scale * rng.uniform(0.7, 1.4) + rng.uniform(0, 6.3))
    return out / 6.0


# ---- the coat's grain ------------------------------------------------------------------------------------------------

def make_grain(path):
    """A fine short-hair grain: thin streaks along the hair's lie (mostly one way, wavering), near white (the coat's
    colour comes from the material and the vertex colours)."""
    size = 512
    rng = np.random.default_rng(3)
    img = np.zeros((size, size))
    for _ in range(9000):
        x, y = rng.uniform(0, size, 2)
        angle = 0.25 + rng.normal(0, 0.12)
        length = rng.uniform(6, 16)
        value = rng.normal(0, 1)
        for t in np.linspace(0, length, int(length * 2)):
            xi, yi = int(x + np.cos(angle) * t) % size, int(y + np.sin(angle) * t) % size
            img[yi, xi] += value
    img = img / max(np.abs(img).max(), 1e-6)
    tone = np.clip(0.9 + 0.1 * img, 0, 1)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray((tone * 255).astype(np.uint8)).convert("RGB").save(path)


# ---- the horse ---------------------------------------------------------------------------------------------------------

def load():
    g = G.Gltf(SRC)
    doc = g.doc
    mats = [m["name"] for m in doc["materials"]]
    parts = {}
    for p in doc["meshes"][0]["primitives"]:
        a = p["attributes"]
        parts[mats[p["material"]]] = {
            "pos": g.read(a["POSITION"]).astype(np.float64), "nor": g.read(a["NORMAL"]).astype(np.float64),
            "joints": g.read(a["JOINTS_0"]).astype(np.uint16), "weights": g.read(a["WEIGHTS_0"]).astype(np.float64),
            "tri": g.read(p["indices"]).astype(np.int64).reshape(-1, 3),
        }
    return g, parts


def smooth_part(part_list, joint_count, levels, tone=None):
    """Welds and subdivides parts together; returns positions, normals, tones, joints, weights, triangles."""
    pos = np.vstack([p["pos"] for p in part_list])
    dense = np.vstack([dense_weights(p["joints"], p["weights"], joint_count) for p in part_list])
    tones = np.vstack([np.full((len(p["pos"]), 1), t) for p, t in zip(part_list, tone or [1.0] * len(part_list))])
    tri, base = [], 0
    for p in part_list:
        tri.append(p["tri"] + base)
        base += len(p["pos"])
    tri = np.vstack(tri)
    (pos, dense, tones), remap = weld(pos, dense, tones)
    tri = remap[tri]
    tri = tri[(tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2]) & (tri[:, 2] != tri[:, 0])]
    for _ in range(levels):
        pos, tri, (dense, tones) = loop_subdivide(pos, tri, [dense, tones])
    nor = G.vertex_normals(pos, tri)
    joints, weights = top4(dense)
    return pos, nor, tones[:, 0], joints, weights, tri


def coat_tone(pos, base):
    """The coat's pattern (multiplies the coat colour): the source's parts, black points below the knees and hocks and
    at the muzzle, a lighter belly, faint dapples."""
    y, z = pos[:, 1], pos[:, 2]
    legs = np.clip((1.45 - y) / 0.5, 0, 1) * (y < 1.95)        # below the knees: dark
    belly = np.clip((y - 1.95) / 0.25, 0, 1) * np.clip((2.45 - y) / 0.3, 0, 1) * (np.abs(z) < 1.2)
    muzzle = np.clip((z - 2.9) / 0.35, 0, 1) * (y > 3.3)
    dapple = noise3(pos, 7.0, 5) * 0.06
    tone = base * (1 - 0.82 * legs) * (1 + 0.18 * belly) * (1 - 0.55 * muzzle) * (1 + dapple)
    return np.clip(tone, 0.05, 1.8)


def shell(pos, nor, joints, weights, tri, keep, offset, smoothing=12):
    """The triangles whose corners all pass `keep`, pushed out along the normals by `offset` (per vertex), their outline
    smoothed (whole triangles make a staircase of it: each outline vertex eased toward its two neighbours on it)."""
    inside = keep[tri].all(axis=1)
    sub = tri[inside]
    used, local = np.unique(sub, return_inverse=True)
    local = local.reshape(-1, 3)
    p = pos[used] + nor[used] * np.asarray(offset)[used][:, None]
    edges = np.sort(np.vstack([local[:, [0, 1]], local[:, [1, 2]], local[:, [2, 0]]]), axis=1)
    uniq, count = np.unique(edges, axis=0, return_counts=True)
    outline = uniq[count == 1]
    if len(outline):
        acc = np.zeros_like(p)
        deg = np.zeros(len(p))
        for _ in range(smoothing):
            acc[:] = 0
            deg[:] = 0
            np.add.at(acc, outline[:, 0], p[outline[:, 1]])
            np.add.at(acc, outline[:, 1], p[outline[:, 0]])
            np.add.at(deg, outline[:, 0], 1)
            np.add.at(deg, outline[:, 1], 1)
            on = deg == 2
            p[on] = 0.5 * p[on] + 0.5 * acc[on] / 2.0
    return p, nor[used], joints[used], weights[used], local


def material(name, color, roughness=0.6, texture=None, double=False):
    m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": 0.0, "roughnessFactor": roughness},
         "doubleSided": double}
    if texture is not None:
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture}
    return m


def build(g, pieces, side, coat, cloth_color):
    doc = copy.deepcopy(g.doc)
    for k in ("meshes", "accessors", "bufferViews", "buffers", "materials", "images", "textures", "samplers"):
        doc.pop(k, None)
    w = G.Writer(doc)
    # The skin's matrices and the clips, written again into this file's buffer.
    skin = doc["skins"][0]
    skin["inverseBindMatrices"] = w.add(g.read(g.doc["skins"][0]["inverseBindMatrices"]).astype(np.float32))
    for a_new, a_old in zip(doc["animations"], g.doc["animations"]):
        for s_new, s_old in zip(a_new["samplers"], a_old["samplers"]):
            s_new["input"] = w.add(g.read(s_old["input"]).astype(np.float32).reshape(-1), minmax=True)
            s_new["output"] = w.add(g.read(s_old["output"]).astype(np.float32))
    doc["images"] = [{"uri": "../../" + GRAIN_REL}]
    doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    doc["textures"] = [{"source": 0, "sampler": 0}]
    doc["materials"] = [
        material("Coat", [min(c * TONE_MAX, 1.0) for c in COATS[coat][:3]] + [1.0], 0.75, texture=0),
        material("Hooves", [0.06, 0.05, 0.045, 1.0], 0.45),
        material("Mane", [0.03, 0.026, 0.024, 1.0] if coat != "Grey" else [0.35, 0.34, 0.33, 1.0], 0.7, double=True),
        material("Eye White", [0.45, 0.45, 0.45, 1.0], 0.3),
        material("Eye", [0.01, 0.01, 0.01, 1.0], 0.1),
        material("Saddle Cloth", cloth_color, 0.85),
        material("Saddle", srgb(70, 40, 22), 0.5),
    ]
    prims = []
    for mat, (pos, nor, col, jn, wt, tri, uv) in pieces.items():
        attrs = {"POSITION": w.add(pos.astype(np.float32), 34962, minmax=True), "NORMAL": w.add(nor.astype(np.float32), 34962),
                 "JOINTS_0": w.add(jn.astype(np.uint16), 34962), "WEIGHTS_0": w.add(wt.astype(np.float32), 34962)}
        if col is not None:
            attrs["COLOR_0"] = w.add(col.astype(np.float32), 34962)
        if uv is not None:
            attrs["TEXCOORD_0"] = w.add(uv.astype(np.float32), 34962)
        prims.append({"attributes": attrs, "indices": w.add(tri.reshape(-1).astype(np.uint32), 34963), "material": mat})
    doc["meshes"] = [{"name": f"Horse {side} {coat}", "primitives": prims}]
    path = os.path.join(OUT, f"Horse_{side}_{coat}.gltf")
    w.save(path)
    S.write_meta(os.path.relpath(path, S.ASSETS).replace("\\", "/"))
    tris = sum(len(p[5]) for p in pieces.values())
    print(f"Horse {side} {coat}: {tris} triangles")


def main():
    grain_path = os.path.join(S.ASSETS, GRAIN_REL)
    make_grain(grain_path)
    S.write_meta(GRAIN_REL, {"filter": 0, "srgb": True, "mips": True})
    g, parts = load()
    joint_count = len(g.doc["skins"][0]["joints"])
    # The body (its coat parts welded together: no seams between them), twice subdivided; mane and tail and hooves once.
    names = [n for n in COAT_PARTS if n in parts]
    pos, nor, tone, jn, wt, tri = smooth_part([parts[n] for n in names], joint_count, 2, [COAT_PARTS[n] for n in names])
    tone = coat_tone(pos, tone)
    tone = tone / TONE_MAX
    col = np.column_stack([tone, tone, tone, np.ones(len(tone))]).clip(0, 1)
    uv = np.column_stack([pos[:, 2], -pos[:, 1]]) * 1.6  # side projection: the grain runs along the body
    pieces = {0: (pos, nor, col, jn, wt, tri, uv)}
    for mat, name, levels in ((1, "Hooves", 1), (2, "Hair", 1)):
        if name in parts:
            p, n, _, j, w_, t = smooth_part([parts[name]], joint_count, levels)
            pieces[mat] = (p, n, None, j, w_, t, None)
    for mat, name in ((3, "Eye_White"), (4, "Eye_Black")):
        if name in parts:
            p = parts[name]
            pieces[mat] = (p["pos"], p["nor"], None, p["joints"], p["weights"].astype(np.float32), p["tri"], None)
    # The saddle cloth: over the back and down the flanks between the withers and the croup; trimmed at its edge.
    y, z = pos[:, 1], pos[:, 2]
    cloth_keep = (z > -0.95) & (z < 0.8) & (y > 2.05)
    cp, cn, cj, cw, ct = shell(pos, nor, jn, wt, tri, cloth_keep, np.full(len(pos), 0.035))
    edge = np.clip(np.minimum.reduce([(cp[:, 1] - 2.05) / 0.12, (cp[:, 2] + 0.95) / 0.12, (0.8 - cp[:, 2]) / 0.12]), 0, 1)
    trim = (1 - edge)[:, None]
    # The war saddle: a raised seat with a high pommel in front and cantle behind.
    seat_keep = (z > -0.5) & (z < 0.45) & (y > 2.75)
    bump = np.exp(-((z - 0.42) / 0.1) ** 2) * 0.22 + np.exp(-((z + 0.47) / 0.1) ** 2) * 0.28
    sp, sn, sj, sw, st = shell(pos, nor, jn, wt, tri, seat_keep, 0.07 + bump * np.clip((y - 2.75) / 0.25, 0, 1))
    for side, colors in SIDES.items():
        # The cloth's material is the brighter of cloth and trim per channel; the vertex colours scale it down to each
        # (so both come out exact and stay within 0-1).
        cloth, trim_color = np.array(colors["cloth"][:3]), np.array(colors["trim"][:3])
        peak = np.maximum(cloth, trim_color)
        rgb = (cloth + (trim_color - cloth) * trim) / peak
        cloth_col = np.column_stack([rgb, np.ones(len(rgb))])
        for coat in COATS:
            pieces_side = dict(pieces)
            pieces_side[5] = (cp, cn, cloth_col, cj, cw, ct, None)
            pieces_side[6] = (sp, sn, None, sj, sw, st, None)
            build(g, pieces_side, side, coat, list(peak) + [1.0])



if __name__ == "__main__":
    main()
