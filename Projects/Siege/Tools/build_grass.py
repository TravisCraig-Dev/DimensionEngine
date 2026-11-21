"""Models/Nature/Grass_Tuft.gltf: a tuft of curved grass blades for the valley's grass field (TerrainComponent's Grass
Mesh, drawn by the GPU around the camera: Grass Distance). Each blade a tapered strip of a few segments bending out
and over, darker at the root, lighter and a little yellower at the tip (vertex colours); one double-sided material.

Run: python build_grass.py, then build_siege.py (or the scene's Valley terrain points at it)."""
import os

import numpy as np

import gltfio as G
import siege as S

OUT_REL = "Models/Nature/Grass_Tuft.gltf"
# The blades' colour (sRGB 92, 118, 44 as linear): near the valley's grass layer, so the field's edge melts into it.
S_GREEN = [float(((c / 255 + 0.055) / 1.055) ** 2.4) for c in (92, 118, 44)] + [1.0]
BLADES = 16
SEGMENTS = 3


def blade(rng):
    """One blade: a strip from the root (at the tuft's middle, spread a little) up and out, its tip bending over."""
    angle = rng.uniform(0, 2 * np.pi)
    out = np.array([np.cos(angle), 0.0, np.sin(angle)])
    side = np.array([-out[2], 0.0, out[0]])
    root = out * rng.uniform(0.0, 0.14)
    height = rng.uniform(0.16, 0.4)
    lean = rng.uniform(0.2, 0.7)         # how far the tip leans out (fraction of the height)
    width = rng.uniform(0.012, 0.022)
    twist = rng.uniform(-0.6, 0.6)
    pos, col = [], []
    for k in range(SEGMENTS + 1):
        t = k / SEGMENTS
        # up and out on a curve that bends over at the top
        p = root + out * (lean * height * t * t) + np.array([0.0, height * (t - 0.18 * t * t * lean), 0.0])
        w = width * (1.0 - t) ** 0.8 + 0.0015
        s = side * np.cos(twist * t) + np.array([0.0, 0.0, 0.0]) + out * np.sin(twist * t) * 0.3
        s /= np.linalg.norm(s)
        for sign in (-1.0, 1.0):
            pos.append(p + s * w * sign)
            shade = 0.45 + 0.55 * t
            col.append([shade * (1.0 + 0.12 * t), shade, shade * (1.0 - 0.15 * t), 1.0])
    # Both windings (the material is one-sided): each face lit with the same up-facing normals, where a double-sided
    # material would turn them down on the back face and darken it.
    tri = []
    for k in range(SEGMENTS):
        a, b, c, d = 2 * k, 2 * k + 1, 2 * k + 2, 2 * k + 3
        tri += [[a, c, b], [b, c, d], [a, b, c], [b, d, c]]
    return np.array(pos), np.array(col), np.array(tri)


def main():
    rng = np.random.default_rng(11)
    pos, col, tri, base = [], [], [], 0
    for _ in range(BLADES):
        p, c, t = blade(rng)
        pos.append(p)
        col.append(c)
        tri.append(t + base)
        base += len(p)
    pos, col, tri = np.vstack(pos), np.vstack(col), np.vstack(tri)
    # Normals mostly up (lit like the ground under them, not edge-on: the blades are thin), a little outward.
    nor = np.column_stack([pos[:, 0] * 1.5, np.ones(len(pos)), pos[:, 2] * 1.5])
    nor /= np.linalg.norm(nor, axis=1, keepdims=True)
    doc = {"asset": {"version": "2.0", "generator": "Siege build_grass"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": "Grass Tuft", "mesh": 0}],
           "materials": [{"name": "Grass Blades", "doubleSided": False,
                          "pbrMetallicRoughness": {"baseColorFactor": S_GREEN, "metallicFactor": 0.0,
                                                   "roughnessFactor": 0.8}}]}
    w = G.Writer(doc)
    doc["meshes"] = [{"name": "Grass Tuft", "primitives": [{"attributes": {
        "POSITION": w.add(pos.astype(np.float32), 34962, minmax=True), "NORMAL": w.add(nor.astype(np.float32), 34962),
        "COLOR_0": w.add(col.clip(0, 1).astype(np.float32), 34962)},
        "indices": w.add(tri.reshape(-1).astype(np.uint32), 34963), "material": 0}]}]
    path = os.path.join(S.ASSETS, OUT_REL)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    w.save(path)
    S.write_meta(OUT_REL)
    print(f"wrote {OUT_REL}: {len(tri)} triangles")


if __name__ == "__main__":
    main()
