"""Builds the soldiers: one skinned glTF per kind and side (Assets/Models/Soldiers/<Side>_<Kind>.gltf).

Each is the Quaternius base body with clothing and armor grown from its own triangles (pushed out along the normals,
skinned like the skin under them), a helmet skinned to the head, and its weapons and shield skinned rigidly to the
hands and forearm, all in one skinned mesh so every soldier of a kind draws as one instance. Animations come from the
Universal Animation Library by bone name (the same skeleton).
"""
import copy
import os

import numpy as np

import anim_author
import gltfio as G
import siege as S

BODY = os.path.join(S.ASSETS, "Models", "Characters", "Superhero_Male_FullBody.gltf")
SHIELD = os.path.join(S.ASSETS, "Models", "Props", "kite_shield", "kite_shield.gltf")
OUT = os.path.join(S.ASSETS, "Models", "Soldiers")
TEX = "../../Textures/Props"  # (from Models/Soldiers)

SIDES = {  # tabard / tunic colors (linear-ish multipliers of the linen texture)
    "Defenders": {"cloth": [0.16, 0.25, 0.55, 1], "trim": [0.85, 0.78, 0.55, 1]},
    "Attackers": {"cloth": [0.55, 0.09, 0.07, 1], "trim": [0.12, 0.11, 0.1, 1]},
}
KINDS = {
    #            mail   tabard  gambeson  helmet    right hand  left hand
    "Swordsman": dict(mail=True, tabard=True, gambeson=False, helmet="nasal", right="sword", left="shield"),
    "Spearman": dict(mail=False, tabard=False, gambeson=True, helmet="kettle", right="spear", left="shield"),
    "Archer": dict(mail=False, tabard=False, gambeson=True, helmet="cap", right=None, left="bow"),
    "Knight": dict(mail=True, tabard=True, gambeson=False, helmet="great", right="sword", left="shield"),
}

TORSO = {"pelvis", "spine_01", "spine_02", "spine_03", "clavicle_l", "clavicle_r"}
UPPER_ARMS = {"upperarm_l", "upperarm_r"}
FOREARMS = {"lowerarm_l", "lowerarm_r"}
THIGHS = {"thigh_l", "thigh_r"}
LEGS = {"calf_l", "calf_r", "foot_l", "foot_r", "ball_l", "ball_r", "ball_leaf_l", "ball_leaf_r"}
HANDS = {n for n in [] }


# ---- loading the body -------------------------------------------------------------------------------------------

def load_body():
    g = G.Gltf(BODY)
    doc = g.doc
    skin = doc["skins"][0]
    names = [doc["nodes"][j]["name"] for j in skin["joints"]]
    ibm = g.read(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    bind = np.linalg.inv(ibm)
    parts = []
    for node in doc["nodes"]:
        if "mesh" not in node:
            continue
        for p in doc["meshes"][node["mesh"]]["primitives"]:
            a = p["attributes"]
            parts.append({
                "name": node["name"], "material": p["material"],
                "pos": g.read(a["POSITION"]).astype(np.float64), "nor": g.read(a["NORMAL"]).astype(np.float64),
                "uv": g.read(a["TEXCOORD_0"]).astype(np.float32),
                "joints": g.read(a["JOINTS_0"]).astype(np.uint16), "weights": g.read(a["WEIGHTS_0"]).astype(np.float32),
                "tri": g.read(p["indices"]).astype(np.uint32).reshape(-1, 3),
            })
    return g, names, bind, parts


# ---- building geometry ------------------------------------------------------------------------------------------

class Part:
    """A piece of the soldier: vertices, triangles, skinning, and the material it uses."""

    def __init__(self, material, pos, nor, uv, tri, joints, weights):
        self.material = material
        self.pos = np.asarray(pos, np.float64)
        self.nor = np.asarray(nor, np.float64)
        self.uv = np.asarray(uv, np.float32)
        self.tri = np.asarray(tri, np.uint32)
        self.joints = np.asarray(joints, np.uint16)
        self.weights = np.asarray(weights, np.float32)


def smooth(pos, tri, iterations, amount=0.5, pinned=None):
    """Laplacian smoothing over the mesh's edges (cloth hides the muscles under it)."""
    n = len(pos)
    edges = np.vstack([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    edges = np.vstack([edges, edges[:, ::-1]])
    deg = np.bincount(edges[:, 0], minlength=n).astype(np.float64)[:, None]
    p = pos.copy()
    for _ in range(iterations):
        avg = np.zeros_like(p)
        np.add.at(avg, edges[:, 0], p[edges[:, 1]])
        avg /= np.maximum(deg, 1)
        moved_p = p + (avg - p) * amount
        if pinned is not None:
            moved_p[pinned] = p[pinned]
        p = moved_p
    return p


def nearest_weights(body, points, k=8):
    """Skin weights for free-standing points (skirts): the weighted blend of the nearest body vertices' weights."""
    joints_out = np.zeros((len(points), 4), np.uint16)
    weights_out = np.zeros((len(points), 4), np.float32)
    bp = body["pos"]
    for i, q in enumerate(points):
        d = np.linalg.norm(bp - q, axis=1)
        near = np.argpartition(d, k)[:k]
        acc = {}
        for v in near:
            w_near = 1.0 / max(d[v], 1e-3)
            for j, w in zip(body["joints"][v], body["weights"][v]):
                if w > 0:
                    acc[int(j)] = acc.get(int(j), 0.0) + w * w_near
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        total = sum(w for _, w in top)
        for slot, (j, w) in enumerate(top):
            joints_out[i, slot] = j
            weights_out[i, slot] = w / total
    return joints_out, weights_out


def shell(body, joint_names, names, offset, material, uv_scale, keep=None, smoothing=0):
    """The body's triangles whose vertices all hang mostly from the given joints, pushed out by `offset` meters
    (and smoothed, so cloth and mail do not show every muscle)."""
    dominant = np.array([names[j] for j in body["joints"][np.arange(len(body["pos"])), body["weights"].argmax(axis=1)]])
    inside = np.isin(dominant, list(joint_names))
    if keep is not None:
        inside &= keep(body["pos"])
    tri = body["tri"][inside[body["tri"]].all(axis=1)]
    used = np.unique(tri)
    remap = np.full(len(body["pos"]), -1, np.int64)
    remap[used] = np.arange(len(used))
    nor_all = G.vertex_normals(body["pos"], body["tri"]).astype(np.float64)
    nor = nor_all[used]
    pos = body["pos"][used] + nor * offset
    local = remap[tri]
    if smoothing:
        smoothed = smooth(pos, local, smoothing)
        # Keep it outside the skin: at least 60% of the offset along the normal.
        depth = np.einsum("ij,ij->i", smoothed - body["pos"][used], nor)
        pos = smoothed + nor * np.maximum(offset * 0.6 - depth, 0)[:, None]
        nor = G.vertex_normals(pos, local).astype(np.float64)
    return Part(material, pos, nor, body["uv"][used] * uv_scale, local, body["joints"][used], body["weights"][used])


def skirt(body, top_y, bottom_y, flare, material, uv_scale, offset=0.03, rows=8, segments=28, front_slit=False):
    """A skirt (a tunic's or hauberk's hem) from the waist at `top_y` down to `bottom_y`, wrapped around the hips at
    `offset` outside the body and flaring by `flare` meters at the hem. Skinned from the nearest body vertices."""
    bp = body["pos"]
    pos, uv = [], []
    for r in range(rows + 1):
        t = r / rows
        y = top_y + (bottom_y - top_y) * t
        band = bp[np.abs(bp[:, 1] - y) < 0.03]
        cx, cz = (band[:, 0].mean(), band[:, 2].mean()) if len(band) else (0.0, 0.0)
        for s in range(segments + 1):
            a = 2 * np.pi * s / segments
            d = np.array([np.sin(a), 0, np.cos(a)])
            # The body's extent in that direction at this height (the outermost vertex of the band near the ray).
            if len(band):
                rel = band - np.array([cx, y, cz])
                along = rel @ d
                lateral = np.linalg.norm(rel - np.outer(along, d), axis=1)
                near = along[lateral < 0.06]
                reach = near.max() if len(near) else np.abs(along).max()
            else:
                reach = 0.15
            hips = band[(np.abs(band[:, 0]) < 0.25)] if len(band) else band
            radius = max(reach, 0.12) + offset + flare * t * t
            pos.append([cx + d[0] * radius, y, cz + d[2] * radius])
            uv.append((s / segments * uv_scale, t * uv_scale * 0.5))
    pos = np.array(pos)
    tri = []
    w = segments + 1
    for r in range(rows):
        for s in range(segments):
            if front_slit and r >= rows // 3 and (s == 0 or s == segments - 1):
                continue
            a, b, c, d = r * w + s, r * w + s + 1, (r + 1) * w + s, (r + 1) * w + s + 1
            tri += [(a, c, b), (b, c, d)]
    tri = np.array(tri)
    # Double-sided by duplicating the triangles reversed (its inside shows when legs swing).
    nor = G.vertex_normals(pos, tri).astype(np.float64)
    joints, weights = nearest_weights(body, pos)
    n = len(pos)
    return Part(material, np.vstack([pos, pos]), np.vstack([nor, -nor]), np.vstack([uv, uv]).astype(np.float32),
                np.vstack([tri, tri[:, ::-1] + n]), np.vstack([joints, joints]), np.vstack([weights, weights]))


def rigid(mesh_pos, mesh_nor, mesh_uv, mesh_tri, transform, joint, material):
    """A rigid mesh placed by `transform` (4x4, bind space) and skinned fully to one joint."""
    p = (transform[:3, :3] @ np.asarray(mesh_pos, np.float64).T).T + transform[:3, 3]
    n = (np.linalg.inv(transform[:3, :3]).T @ np.asarray(mesh_nor, np.float64).T).T
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    count = len(p)
    joints = np.zeros((count, 4), np.uint16)
    joints[:, 0] = joint
    weights = np.zeros((count, 4), np.float32)
    weights[:, 0] = 1
    return Part(material, p, n, mesh_uv, mesh_tri, joints, weights)


def box(sx, sy, sz, uv_scale=1.0):
    """A box centered at the origin: 24 vertices with flat normals."""
    pos, nor, uv, tri = [], [], [], []
    for axis in range(3):
        for sign in (-1, 1):
            n = np.zeros(3)
            n[axis] = sign
            u = np.zeros(3)
            u[(axis + 1) % 3] = 1
            v = np.cross(n, u)
            half = np.array([sx, sy, sz]) / 2
            base = len(pos)
            for cu, cv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                pos.append(n * half + u * half * cu + v * half * cv)
                nor.append(n)
                uv.append(((cu + 1) / 2 * uv_scale, (cv + 1) / 2 * uv_scale))
            tri += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return np.array(pos), np.array(nor), np.array(uv, np.float32), np.array(tri)


def cylinder(radius, length, segments=10, radius_top=None, uv_scale=1.0, caps=True):
    """A cylinder (or cone) along +Y from 0 to `length`."""
    rt = radius if radius_top is None else radius_top
    pos, nor, uv, tri = [], [], [], []
    for i in range(segments + 1):
        a = 2 * np.pi * i / segments
        d = np.array([np.cos(a), 0, np.sin(a)])
        slope = (radius - rt) / max(length, 1e-6)
        n = d + np.array([0, slope, 0])
        n /= np.linalg.norm(n)
        pos += [d * radius, d * rt + np.array([0, length, 0])]
        nor += [n, n]
        uv += [(i / segments * uv_scale, 0), (i / segments * uv_scale, length * uv_scale)]
    for i in range(segments):
        a, b, c, d = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
        tri += [(a, b, d), (a, d, c)]
    if caps:
        for y, r, s in ((0, radius, -1), (length, rt, 1)):
            center = len(pos)
            pos.append([0, y, 0])
            nor.append([0, s, 0])
            uv.append((0.5, 0.5))
            for i in range(segments + 1):
                a = 2 * np.pi * i / segments
                pos.append([np.cos(a) * r, y, np.sin(a) * r])
                nor.append([0, s, 0])
                uv.append((0.5 + np.cos(a) * 0.5, 0.5 + np.sin(a) * 0.5))
            for i in range(segments):
                t = (center, center + 1 + i, center + 2 + i)
                tri.append(t if s < 0 else (t[0], t[2], t[1]))
    return np.array(pos, np.float64), np.array(nor, np.float64), np.array(uv, np.float32), np.array(tri)


def merge(*meshes):
    pos, nor, uv, tri = [], [], [], []
    base = 0
    for m in meshes:
        pos.append(m[0])
        nor.append(m[1])
        uv.append(m[2])
        tri.append(np.asarray(m[3]) + base)
        base += len(m[0])
    return np.vstack(pos), np.vstack(nor), np.vstack(uv), np.vstack(tri)


def moved(mesh, offset=(0, 0, 0), rotation=None):
    r = np.eye(3) if rotation is None else rotation
    return ((r @ mesh[0].T).T + np.array(offset), (r @ mesh[1].T).T, mesh[2], mesh[3])


def rot_x(deg):
    a = np.radians(deg)
    return np.array([[1, 0, 0], [0, np.cos(a), -np.sin(a)], [0, np.sin(a), np.cos(a)]])


def rot_y(deg):
    a = np.radians(deg)
    return np.array([[np.cos(a), 0, np.sin(a)], [0, 1, 0], [-np.sin(a), 0, np.cos(a)]])


def rot_z(deg):
    a = np.radians(deg)
    return np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])


def dome(radius, y_from, y_to, segments=16, rings=8, scale=(1, 1, 1)):
    """A spherical band (a helmet bowl) from height y_from to y_to (fractions of the radius, -1..1), open below."""
    pos, nor, uv, tri = [], [], [], []
    t0, t1 = np.arcsin(np.clip(y_from, -1, 1)), np.pi / 2
    for r in range(rings + 1):
        t = t0 + (t1 - t0) * r / rings
        for s in range(segments + 1):
            a = 2 * np.pi * s / segments
            n = np.array([np.cos(t) * np.cos(a), np.sin(t), np.cos(t) * np.sin(a)])
            pos.append(n * radius * np.array(scale))
            nn = n / np.array(scale)
            nor.append(nn / np.linalg.norm(nn))
            uv.append((s / segments, r / rings))
    w = segments + 1
    for r in range(rings):
        for s in range(segments):
            a, b = r * w + s, r * w + s + 1
            c, d = a + w, b + w
            tri += [(a, c, b), (b, c, d)]
    return np.array(pos), np.array(nor), np.array(uv, np.float32), np.array(tri)


# ---- equipment ----------------------------------------------------------------------------------------------------

def sword():
    blade = cylinder(0.022, 0.82, segments=4, radius_top=0.004, caps=False)
    blade = (blade[0] * np.array([1.0, 1, 0.22]), blade[1], blade[2], blade[3])  # flat, diamond section
    return {
        "steel": merge(moved(blade, (0, 0.11, 0))),
        "iron": merge(moved(box(0.2, 0.025, 0.035), (0, 0.105, 0)), moved(cylinder(0.022, 0.03), (0, -0.115, 0))),
        "leather": merge(moved(cylinder(0.016, 0.2, segments=8), (0, -0.09, 0))),
    }


def spear():
    head = cylinder(0.025, 0.28, segments=4, radius_top=0.002, caps=False)
    head = (head[0] * np.array([1.0, 1, 0.3]), head[1], head[2], head[3])
    return {
        "wood": merge(moved(cylinder(0.017, 2.5, segments=8, uv_scale=4.0), (0, -0.85, 0))),
        "steel": merge(moved(head, (0, 1.65, 0))),
        "iron": merge(moved(cylinder(0.02, 0.08, segments=8), (0, 1.6, 0))),
    }


def bow():
    # A longbow: a curved stave of short tapered segments (in the bind pose's hand frame: along Y).
    segs = []
    n = 14
    for i in range(n):
        t0, t1 = i / n, (i + 1) / n
        y0, y1 = (t0 - 0.5) * 1.7, (t1 - 0.5) * 1.7
        # the grip (the stave's middle) in the fist, the tips 0.18 m back toward the string
        z0, z1 = 0.18 * (2 * t0 - 1) ** 2, 0.18 * (2 * t1 - 1) ** 2
        r = 0.016 - 0.01 * abs(t0 - 0.5)
        d = np.array([0, y1 - y0, z1 - z0])
        length = np.linalg.norm(d)
        ang = np.degrees(np.arctan2(d[2], d[1]))
        segs.append(moved(cylinder(r, length, segments=6), (0, y0, z0), rot_x(ang)))
    # The string in two halves meeting at the middle (where the draw pulls it: the bowstring joint).
    back = anim_author.STRING_BACK
    upper = moved(cylinder(0.002, 0.85, segments=3, caps=False), (0, 0, back))
    lower = moved(cylinder(0.002, 0.85, segments=3, caps=False), (0, -0.85, back))
    return {"wood": merge(*segs), "leather": merge(upper, lower)}


# ---- materials ----------------------------------------------------------------------------------------------------

def texture_material(doc, name, folder, color, metallic=0.0, uv=None):
    """A glTF material using a Poly Haven texture set (color, normal, ARM)."""
    def image(suffix):
        uri = f"{TEX}/{folder}/{folder}_{suffix}_2k.jpg"
        for i, img in enumerate(doc["images"]):
            if img.get("uri") == uri:
                break
        else:
            doc["images"].append({"uri": uri})
            i = len(doc["images"]) - 1
        doc["textures"].append({"source": i})
        return len(doc["textures"]) - 1

    doc["materials"].append({
        "name": name,
        "pbrMetallicRoughness": {"baseColorFactor": color, "baseColorTexture": {"index": image("diff")},
                                 "metallicFactor": metallic, "roughnessFactor": 1.0,
                                 "metallicRoughnessTexture": {"index": image("arm")}},
        "normalTexture": {"index": image("nor_gl")},
        "occlusionTexture": {"index": image("arm")},
    })
    return len(doc["materials"]) - 1


# ---- assembly -----------------------------------------------------------------------------------------------------

def build(side, kind, spec, body_gltf, names, bind, body_parts, shield_parts):
    doc = copy.deepcopy(body_gltf.doc)
    names = list(names)  # (an archer adds its bowstring joint)
    for k in ("meshes", "accessors", "bufferViews", "buffers", "animations"):
        doc.pop(k, None)
    for img in doc["images"]:  # body textures stay beside the body model, the shield's beside the shield
        uri = img["uri"]
        img["uri"] = uri[len("__SHIELD__"):] if uri.startswith("__SHIELD__") else "../Characters/" + uri
    colors = SIDES[side]
    mats = {
        "cloth": texture_material(doc, f"{side} Cloth", "linen", colors["cloth"]),
        "trim": texture_material(doc, f"{side} Trim", "linen", colors["trim"]),
        "gambeson": texture_material(doc, "Gambeson", "linen", [0.5, 0.38, 0.24, 1]),
        "hose": texture_material(doc, "Hose", "linen", [0.12, 0.1, 0.08, 1]),
        "mail": texture_material(doc, "Mail", "chainmail", [0.85, 0.85, 0.87, 1], metallic=1.0),
        "steel": texture_material(doc, "Steel", "metal_plate", [0.8, 0.8, 0.82, 1], metallic=1.0),
        "iron": texture_material(doc, "Iron", "rusty_metal_02", [0.45, 0.43, 0.42, 1], metallic=0.8),
        "leather": texture_material(doc, "Leather", "brown_leather", [0.7, 0.6, 0.5, 1]),
        "wood": texture_material(doc, "Wood", "rough_wood", [0.85, 0.75, 0.62, 1]),
    }
    skin_body = next(p for p in body_parts if p["name"] == "SuperHero_Male")
    joint = {n: i for i, n in enumerate(names)}
    parts = []
    for p in body_parts:
        if p["name"] == "Eyebrows" and spec["helmet"] == "great":
            continue
        parts.append(Part(p["material"], p["pos"], p["nor"], p["uv"], p["tri"], p["joints"], p["weights"]))

    # Clothing: shoes, gloves and wool hose always; a padded gambeson or a mail hauberk (with its skirt) and a
    # tabard or tunic by kind. Smoothed, so no muscles show through.
    parts.append(shell(skin_body, LEGS, names, 0.018, mats["leather"], 6.0, keep=lambda p: p[:, 1] < 0.36, smoothing=12))
    hands = {n for n in names if n.endswith(("_l", "_r")) and n.split("_")[0] in
             ("hand", "index", "middle", "ring", "pinky", "thumb")}
    parts.append(shell(skin_body, hands, names, 0.006, mats["leather"], 8.0, smoothing=4))
    parts.append(shell(skin_body, THIGHS | {"calf_l", "calf_r", "pelvis"}, names, 0.01, mats["hose"], 6.0,
                       keep=lambda p: (p[:, 1] >= 0.3) & (p[:, 1] < 1.0), smoothing=10))
    upper = TORSO | UPPER_ARMS | FOREARMS | {"neck_01"}
    if spec["mail"]:
        parts.append(shell(skin_body, upper | {"pelvis"}, names, 0.02, mats["mail"], 12.0,
                           keep=lambda p: (p[:, 1] > 0.86) & (p[:, 1] < 1.6), smoothing=14))
        parts.append(skirt(skin_body, 0.98, 0.62, 0.06, mats["mail"], 12.0, offset=0.025, front_slit=True))
        # A mail coif around the head and neck, leaving the face open (under the helmet).
        head_y = bind[joint["Head"]][1, 3]
        face_z = bind[joint["Head"]][2, 3] + 0.045
        parts.append(shell(skin_body, {"Head", "neck_01"}, names, 0.014, mats["mail"], 12.0, smoothing=8,
                           keep=lambda p: ~((p[:, 2] > face_z) & (p[:, 1] > head_y - 0.06) & (p[:, 1] < head_y + 0.15))))
    if spec["gambeson"]:
        parts.append(shell(skin_body, upper | {"pelvis"}, names, 0.03, mats["gambeson"], 6.0,
                           keep=lambda p: (p[:, 1] > 0.86) & (p[:, 1] < 1.58), smoothing=18))
        parts.append(skirt(skin_body, 0.98, 0.58, 0.07, mats["gambeson"], 6.0, offset=0.034, front_slit=True))
    if spec["tabard"]:
        parts.append(shell(skin_body, TORSO | {"pelvis"}, names, 0.034, mats["cloth"], 5.0,
                           keep=lambda p: (p[:, 1] > 0.86) & (p[:, 1] < 1.5), smoothing=18))
        parts.append(skirt(skin_body, 0.97, 0.5, 0.1, mats["cloth"], 5.0, offset=0.045, front_slit=True))
    elif spec["gambeson"]:
        # A sleeveless surcoat in the side's color over the gambeson.
        parts.append(shell(skin_body, TORSO, names, 0.042, mats["cloth"], 5.0,
                           keep=lambda p: (p[:, 1] > 0.9) & (p[:, 1] < 1.46), smoothing=18))

    # Helmet on the head joint.
    head = bind[joint["Head"]]
    head_top = skin_body["pos"][skin_body["weights"].argmax(axis=1) >= 0][:, 1].max()
    center = np.array([0, head_top - 0.115, head[2, 3] + 0.015])
    helm = {
        "nasal": merge(moved(dome(0.122, 0.05, 1, scale=(1, 1.08, 1.08)), center),
                       moved(box(0.022, 0.09, 0.012), center + np.array([0, -0.02, 0.128]))),
        "kettle": merge(moved(dome(0.122, 0.1, 1, scale=(1, 0.95, 1.08)), center),
                        moved(cylinder(0.2, 0.012, segments=20, radius_top=0.18), center + np.array([0, 0.005, 0]))),
        "cap": merge(moved(dome(0.118, 0.15, 1, scale=(1, 0.9, 1.06)), center)),
        "great": merge(moved(cylinder(0.125, 0.27, segments=18, radius_top=0.118), center + np.array([0, -0.15, 0.0])),
                       moved(dome(0.118, 0, 1, scale=(1, 0.55, 1.0)), center + np.array([0, 0.12, 0]))),
    }[spec["helmet"]]
    helm_material = mats["leather"] if spec["helmet"] == "cap" else mats["steel"]
    parts.append(rigid(*helm, np.eye(4), joint["Head"], helm_material))

    # Weapons in the right hand: the grip across the closed fist (bind pose: along +Z), the blade forward.
    hand_r = bind[joint["hand_r"]]
    grip = np.eye(4)
    grip[:3, :3] = rot_x(90)  # the meshes' +Y (blade) to +Z (forward)
    grip[:3, 3] = hand_r[:3, 3] + np.array([-0.085, -0.02, 0.0])
    if spec["right"] == "sword":
        for mat, mesh in sword().items():
            parts.append(rigid(*mesh, grip, joint["hand_r"], mats[mat]))
    elif spec["right"] == "spear":
        for mat, mesh in spear().items():
            parts.append(rigid(*mesh, grip, joint["hand_r"], mats[mat]))

    # Left hand: a bow held upright in the fist, or the kite shield on the forearm (its face forward).
    hand_l = bind[joint["hand_l"]]
    if spec["left"] == "bow":
        hold = np.eye(4)
        hold[:3, :3] = np.eye(3)
        hold[:3, 3] = hand_l[:3, 3] + anim_author.BOW_HOLD
        string_joint = add_bowstring(doc, names, bind)
        arrow_joint, arrow_bind = add_nockedarrow(doc, names, bind)
        aim = np.eye(4)
        aim[:3, :3] = rot_x(-90)  # the meshes' +Y to -Z: from the nock through the bow
        aim[:3, 3] = arrow_bind[:3, 3]
        for mat, mesh in nocked_arrow().items():
            parts.append(rigid(*mesh, aim, arrow_joint, mats[mat]))
        for mat, mesh in bow().items():
            part = rigid(*mesh, hold, joint["hand_l"], mats[mat])
            if mat == "leather":  # the string: its middle on the bowstring joint (the draw pulls it back)
                middle = np.linalg.norm(part.pos - (hold[:3, 3] + [0, 0, anim_author.STRING_BACK]), axis=1) < 0.01
                part.joints[middle, 0] = string_joint
            parts.append(part)
    elif spec["left"] == "shield":
        forearm = bind[joint["lowerarm_l"]]
        place = np.eye(4)
        # Strapped across the forearm, its face on the back of the arm (bind pose: palm down, so up) and its top
        # toward the thumb (forward): held in guard, upright with the point down.
        place[:3, :3] = rot_x(90) @ rot_y(180)
        place[:3, 3] = forearm[:3, 3] + np.array([0.13, 0.075, 0.02])
        for sp in shield_parts:
            parts.append(rigid(sp["pos"], sp["nor"], sp["uv"], sp["tri"], place, joint["lowerarm_l"], sp["material"]))

    # The shield's materials and textures come along (its own folder).
    write(doc, parts, side, kind, names)


def load_shield():
    g = G.Gltf(SHIELD)
    W = G.world_matrices(g.doc)
    parts = []
    for i, node in enumerate(g.doc["nodes"]):
        if "mesh" not in node:
            continue
        for p in g.doc["meshes"][node["mesh"]]["primitives"]:
            a = p["attributes"]
            pos = g.read(a["POSITION"]).astype(np.float64)
            nor = g.read(a["NORMAL"]).astype(np.float64)
            m = W[i]
            pos = (m[:3, :3] @ pos.T).T + m[:3, 3]
            nor = (m[:3, :3] @ nor.T).T
            parts.append({"pos": pos, "nor": nor, "uv": g.read(a["TEXCOORD_0"]), "tri": g.read(p["indices"]).reshape(-1, 3),
                          "material": p["material"]})
    allp = np.vstack([p["pos"] for p in parts])
    lo, hi = allp.min(axis=0), allp.max(axis=0)
    center = (lo + hi) / 2
    for p in parts:
        p["pos"] = p["pos"] - center
    return g, parts, hi - lo


def add_bowstring(doc, names, bind):
    """A joint for the middle of the bowstring under the left hand (anim_author.py animates it); returns its index."""
    hand = next(i for i, n in enumerate(doc["nodes"]) if n.get("name") == "hand_l")
    hand_bind = bind[names.index("hand_l")]
    local = anim_author.bowstring_local(hand_bind)
    doc["nodes"].append({"name": "bowstring", "translation": local.tolist()})
    doc["nodes"][hand].setdefault("children", []).append(len(doc["nodes"]) - 1)
    doc["skins"][0]["joints"].append(len(doc["nodes"]) - 1)
    names.append("bowstring")
    world = hand_bind.copy()
    world[:3, 3] = (hand_bind @ np.append(local, 1.0))[:3]
    EXTRA_BIND.append(np.linalg.inv(world).T.astype(np.float32))  # (column-major, as the file stores them)
    return len(names) - 1


def add_nockedarrow(doc, names, bind):
    """The nocked arrow's joint beside the bowstring's (anim_author.py shows it while drawing); returns its index and
    its bind (world)."""
    hand = next(i for i, n in enumerate(doc["nodes"]) if n.get("name") == "hand_l")
    hand_bind = bind[names.index("hand_l")]
    local, rotation = anim_author.nockedarrow_bind(hand_bind)
    doc["nodes"].append({"name": "nockedarrow", "translation": local.tolist(), "rotation": rotation.tolist(),
                         "scale": [anim_author.ARROW_HIDDEN] * 3})
    doc["nodes"][hand]["children"].append(len(doc["nodes"]) - 1)
    doc["skins"][0]["joints"].append(len(doc["nodes"]) - 1)
    names.append("nockedarrow")
    world = np.eye(4)
    world[:3, 3] = (hand_bind @ np.append(local, 1.0))[:3]
    EXTRA_BIND.append(np.linalg.inv(world).T.astype(np.float32))
    return len(names) - 1, world


def nocked_arrow():
    """An arrow along +Y from its nock: a shaft, a bodkin head and three fletches (as Models/Props/Arrow.gltf)."""
    length = anim_author.ARROW_LENGTH
    shaft = cylinder(0.0045, length - 0.05, segments=5, uv_scale=0.5)
    head = moved(cylinder(0.008, 0.06, segments=4, radius_top=0.0005), (0, length - 0.06, 0))
    vanes = []
    for k in range(3):
        vane = moved(box(0.0008, 0.11, 0.016), (0, 0.09, 0.011))
        vanes.append(moved(vane, (0, 0, 0), rot_y(120 * k)))
    return {"wood": merge(shaft), "steel": merge(head), "trim": merge(*vanes)}


EXTRA_BIND = []


def write(doc, parts, side, kind, names):
    w = G.Writer(doc)
    prims = []
    by_material = {}
    for p in parts:
        by_material.setdefault(p.material, []).append(p)
    for material, group in by_material.items():
        pos = np.vstack([p.pos for p in group]).astype(np.float32)
        nor = np.vstack([p.nor for p in group]).astype(np.float32)
        uv = np.vstack([p.uv for p in group]).astype(np.float32)
        jn = np.vstack([p.joints for p in group]).astype(np.uint16)
        wt = np.vstack([p.weights for p in group]).astype(np.float32)
        wt /= np.maximum(wt.sum(axis=1, keepdims=True), 1e-6)
        tri, base = [], 0
        for p in group:
            tri.append(p.tri.astype(np.uint32) + base)
            base += len(p.pos)
        tri = np.vstack(tri).reshape(-1).astype(np.uint32)
        prims.append({
            "attributes": {"POSITION": w.add(pos, 34962, minmax=True), "NORMAL": w.add(nor, 34962),
                           "TEXCOORD_0": w.add(uv, 34962), "JOINTS_0": w.add(jn, 34962),
                           "WEIGHTS_0": w.add(wt, 34962)},
            "indices": w.add(tri, 34963), "material": material})
    doc["meshes"] = [{"name": f"{side} {kind}", "primitives": prims}]
    # One mesh node with the skin; the armature's other mesh nodes go.
    mesh_nodes = [i for i, n in enumerate(doc["nodes"]) if "mesh" in n]
    keep = mesh_nodes[0]
    for i in mesh_nodes[1:]:
        doc["nodes"][i].pop("mesh", None)
        doc["nodes"][i].pop("skin", None)
    doc["nodes"][keep]["mesh"] = 0
    doc["nodes"][keep]["skin"] = 0
    doc["nodes"][keep]["name"] = f"{side} {kind}"
    # The skin's inverse bind matrices, written again.
    ibm = INVERSE_BIND if not EXTRA_BIND else np.concatenate([INVERSE_BIND.reshape(-1, 4, 4), np.array(EXTRA_BIND)]).reshape(len(INVERSE_BIND) + len(EXTRA_BIND), -1)
    EXTRA_BIND.clear()
    w.doc["skins"][0]["inverseBindMatrices"] = w.add(ibm.astype(np.float32))
    path = os.path.join(OUT, f"{side}_{kind}.gltf")
    w.save(path)
    S.write_meta(os.path.relpath(path, S.ASSETS).replace("\\", "/"))
    tris = sum(len(p.tri) for p in parts)
    print(f"{side} {kind}: {tris} triangles, {len(prims)} materials")


INVERSE_BIND = None


def main():
    global INVERSE_BIND
    body, names, bind, parts = load_body()
    INVERSE_BIND = body.read(body.doc["skins"][0]["inverseBindMatrices"]).astype(np.float32)
    shield_gltf, shield_parts, size = load_shield()
    for side in SIDES:
        for kind, spec in KINDS.items():
            doc_shield = None
            build_with_shield(side, kind, spec, body, names, bind, parts, shield_gltf, shield_parts)


def build_with_shield(side, kind, spec, body, names, bind, parts, shield_gltf, shield_parts):
    # The shield's materials join the soldier's: copy them (with their textures, by path) and renumber.
    shield_doc = shield_gltf.doc
    original = copy.deepcopy(body.doc)
    img0, tex0, mat0 = len(original["images"]), len(original["textures"]), len(original["materials"])
    body.doc["images"] += [{"uri": "__SHIELD__../Props/kite_shield/" + i["uri"]} for i in shield_doc["images"]]
    for t in shield_doc["textures"]:
        body.doc["textures"].append({"source": t["source"] + img0})
    for m in shield_doc["materials"]:
        m = copy.deepcopy(m)
        for key in ("baseColorTexture", "metallicRoughnessTexture"):
            if key in m.get("pbrMetallicRoughness", {}):
                m["pbrMetallicRoughness"][key]["index"] += tex0
        for key in ("normalTexture", "occlusionTexture", "emissiveTexture"):
            if key in m:
                m[key]["index"] += tex0
        body.doc["materials"].append(m)
    shifted = [dict(p, material=p["material"] + mat0) for p in shield_parts]
    build(side, kind, spec, body, names, bind, parts, shifted)
    body.doc = original


if __name__ == "__main__":
    main()
