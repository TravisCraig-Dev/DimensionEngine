"""Conifers for the forests: a bark trunk and drooping branch cards in whorls (a few hundred triangles, so thousands
can stand on the mountain), with a painted fir-branch card texture (twigs and needles, alpha-tested)."""
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import gltfio as G
import siege as S

OUT = os.path.join(S.ASSETS, "Models", "Trees")
TEX = os.path.join(S.ASSETS, "Textures", "Trees")


def branch_texture(size=(512, 1024), seed=3):
    """A fir branch seen from above: a twig up the middle, side twigs, dense needles. RGBA with a normal map."""
    w, h = size
    rng = np.random.default_rng(seed)
    color = Image.new("RGBA", size, (0, 0, 0, 0))
    height = Image.new("L", size, 0)
    dc, dh = ImageDraw.Draw(color), ImageDraw.Draw(height)

    def needles(x0, y0, x1, y1, length, density):
        n = int(np.hypot(x1 - x0, y1 - y0) * density)
        for i in range(n):
            t = rng.random()
            px, py = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            ang = np.arctan2(y1 - y0, x1 - x0) + rng.choice([-1, 1]) * rng.uniform(0.6, 1.3)
            ln = length * rng.uniform(0.6, 1.1) * (1 - 0.4 * t)
            ex, ey = px + np.cos(ang) * ln, py + np.sin(ang) * ln
            g = rng.uniform(0.55, 1.0)
            dc.line([(px, py), (ex, ey)], fill=(int(28 * g), int(58 * g), int(30 * g), 255), width=2)
            dh.line([(px, py), (ex, ey)], fill=int(150 + 100 * g), width=2)

    cx = w / 2
    dc.line([(cx, h - 4), (cx, 8)], fill=(70, 52, 36, 255), width=7)
    dh.line([(cx, h - 4), (cx, 8)], fill=255, width=7)
    for k in range(22):
        y = h - 50 - k * (h - 100) / 22
        for side in (-1, 1):
            span = (w * 0.46) * (1 - k / 30) * rng.uniform(0.85, 1.0)
            ex, ey = cx + side * span, y - span * 0.55
            dc.line([(cx, y), (ex, ey)], fill=(60, 46, 32, 255), width=3)
            dh.line([(cx, y), (ex, ey)], fill=220, width=3)
            needles(cx, y, ex, ey, 30, 1.8)
    needles(cx, h - 10, cx, 10, 34, 2.0)
    alpha = np.array(color)[:, :, 3]
    rgb = np.array(color)[:, :, :3]
    # Spread the needle colors into the transparent texels so mip levels don't fringe dark.
    blurred = np.array(color.filter(ImageFilter.GaussianBlur(6)))[:, :, :3]
    rgb = np.where(alpha[..., None] > 0, rgb, blurred)
    hgt = np.array(height.filter(ImageFilter.GaussianBlur(1)), dtype=np.float64) / 255
    gy, gx = np.gradient(hgt)
    n = np.dstack([-gx * 6, gy * 6, np.ones_like(hgt)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return Image.fromarray(np.dstack([rgb, alpha]).astype(np.uint8), "RGBA"), Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8))


def fir(height, seed):
    """Trunk + whorls of drooping cards. Returns (trunk mesh, card mesh) as (pos, nor, uv, tri)."""
    rng = np.random.default_rng(seed)
    # Trunk: a tapered cylinder.
    segs = 7
    pos, nor, uv, tri = [], [], [], []
    for ring, y in enumerate((0.0, height * 0.5, height)):
        r = 0.18 * height / 14 * (1 - ring / 2.2)
        for s in range(segs + 1):
            a = 2 * np.pi * s / segs
            pos.append([np.cos(a) * r, y, np.sin(a) * r])
            nor.append([np.cos(a), 0, np.sin(a)])
            uv.append([s / segs, y / 2.0])
    for ring in range(2):
        for s in range(segs):
            a, b = ring * (segs + 1) + s, ring * (segs + 1) + s + 1
            tri += [(a, a + segs + 1, b), (b, a + segs + 1, b + segs + 1)]
    trunk = (np.array(pos), np.array(nor), np.array(uv, np.float32), np.array(tri))
    # Branch whorls: cards from the trunk outward, drooping, shorter toward the top.
    pos, nor, uv, tri = [], [], [], []
    whorls = 16
    for k in range(whorls):
        t = k / (whorls - 1)
        y = height * (0.18 + 0.8 * t)
        length = height * 0.34 * (1 - t) ** 0.9 + 0.35
        count = 9 if t < 0.7 else 6
        for b in range(count):
            a = 2 * np.pi * (b + rng.random() * 0.6) / count + k * 0.7
            d = np.array([np.cos(a), 0, np.sin(a)])
            side = np.array([-d[2], 0, d[0]])
            droop = -0.35 - 0.25 * (1 - t)
            tip = np.array([0, y, 0]) + d * length + np.array([0, droop * length, 0])
            root = np.array([0, y + 0.1, 0])
            width = length * 0.7
            up = np.cross(tip - root, side)
            up /= np.linalg.norm(up)
            if up[1] < 0:
                up = -up
            base = len(pos)
            for (p, u, v) in ((root - side * width * 0.25, 0, 0), (root + side * width * 0.25, 1, 0),
                              (tip + side * width * 0.5, 1, 1), (tip - side * width * 0.5, 0, 1)):
                pos.append(p)
                nor.append(up * 0.7 + d * 0.3)  # (soft, roundish canopy lighting)
                uv.append([u, 1 - v])
            tri += [(base, base + 2, base + 1), (base, base + 3, base + 2)]
    nor = np.array(nor)
    nor /= np.linalg.norm(nor, axis=1, keepdims=True)
    cards = (np.array(pos), nor, np.array(uv, np.float32), np.array(tri))
    return trunk, cards


def write_tree(name, trunk, cards):
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": name, "mesh": 0}],
           "images": [{"uri": "../../Models/Nature/pine_sapling_small/textures/pine_sapling_small_bark_diff_1k.jpg"},
                      {"uri": "../../Models/Nature/pine_sapling_small/textures/pine_sapling_small_bark_nor_gl_1k.jpg"},
                      {"uri": "../../Textures/Trees/fir_branch_diff.png"},
                      {"uri": "../../Textures/Trees/fir_branch_nor_gl.png"}],
           "textures": [{"source": i} for i in range(4)],
           "materials": [
               {"name": "Bark", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0,
                                                         "roughnessFactor": 0.9}, "normalTexture": {"index": 1}},
               {"name": "Fir Branches", "alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": True,
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 2}, "baseColorFactor": [0.9, 1.0, 0.9, 1],
                                         "metallicFactor": 0, "roughnessFactor": 0.85}, "normalTexture": {"index": 3}}]}
    w = G.Writer(doc)
    prims = []
    for mat, (p, n, uv, tri) in enumerate((trunk, cards)):
        prims.append({"attributes": {"POSITION": w.add(p.astype(np.float32), 34962, minmax=True),
                                     "NORMAL": w.add(n.astype(np.float32), 34962),
                                     "TEXCOORD_0": w.add(uv.astype(np.float32), 34962)},
                      "indices": w.add(np.asarray(tri, np.uint32).reshape(-1), 34963), "material": mat})
    doc["meshes"] = [{"name": name, "primitives": prims}]
    path = os.path.join(OUT, f"{name}.gltf")
    w.save(path)
    S.write_meta(os.path.relpath(path, S.ASSETS).replace("\\", "/"))


def main():
    os.makedirs(TEX, exist_ok=True)
    color, normal = branch_texture()
    color.save(os.path.join(TEX, "fir_branch_diff.png"))
    normal.save(os.path.join(TEX, "fir_branch_nor_gl.png"))
    S.write_meta("Textures/Trees/fir_branch_diff.png")
    S.write_meta("Textures/Trees/fir_branch_nor_gl.png")
    for name, h, seed in (("Fir_A", 14.0, 1), ("Fir_B", 18.0, 2)):
        write_tree(name, *fir(h, seed))
        print("wrote", name)


if __name__ == "__main__":
    main()
