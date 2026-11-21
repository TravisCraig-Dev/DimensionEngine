"""Building blocks for the procedural architecture (castle, siege engines, palisades): faces, boxes, cylinders, cones
and roofs gathered per material, with UVs in meters (each material's texture covers `tile` meters), written as one
glTF whose materials use the Poly Haven texture sets (diffuse, OpenGL normal, AO/roughness/metal)."""
import os

import numpy as np

import gltfio as G
import siege as S


def rot_x(deg):
    a = np.radians(deg)
    m = np.eye(4)
    m[1, 1], m[1, 2], m[2, 1], m[2, 2] = np.cos(a), -np.sin(a), np.sin(a), np.cos(a)
    return m


def rot_z(deg):
    a = np.radians(deg)
    m = np.eye(4)
    m[0, 0], m[0, 1], m[1, 0], m[1, 1] = np.cos(a), -np.sin(a), np.sin(a), np.cos(a)
    return m


def translate(x, y, z):
    m = np.eye(4)
    m[:3, 3] = (x, y, z)
    return m


def _norm(v):
    v = np.asarray(v, np.float64)
    return v / max(np.linalg.norm(v), 1e-12)


class Material:
    def __init__(self, name, texture_folder=None, tile=2.0, color=(1, 1, 1, 1), roughness=1.0, metallic=0.0,
                 double_sided=False, emissive=None):
        self.name, self.folder, self.tile, self.color = name, texture_folder, tile, color
        self.roughness, self.metallic, self.double_sided, self.emissive = roughness, metallic, double_sided, emissive


class Kit:
    def __init__(self):
        self.parts = {}  # material name -> [positions, normals, uvs, triangles]
        self.materials = {}

    def material(self, mat):
        self.materials[mat.name] = mat
        return mat

    def _buf(self, mat):
        self.materials.setdefault(mat.name, mat)
        return self.parts.setdefault(mat.name, [[], [], [], []])

    # --- primitives -------------------------------------------------------------------------------------------------

    def poly(self, mat, pts, normal=None, uv_origin=None, u_axis=None, normals=None, uvs=None):
        """A flat convex polygon (counter-clockwise seen from its front). UVs project onto its plane in meters."""
        pts = np.asarray(pts, np.float64)
        n = _norm(np.cross(pts[1] - pts[0], pts[2] - pts[0])) if normal is None else _norm(normal)
        if uvs is None:
            u = _norm(pts[1] - pts[0]) if u_axis is None else _norm(u_axis)
            if abs(n[1]) < 0.7:  # walls: u horizontal, v up (texture upright)
                u = _norm(np.cross([0, 1, 0], n))
            v = np.cross(n, u)
            o = pts[0] if uv_origin is None else np.asarray(uv_origin, np.float64)
            uvs = [[np.dot(p - o, u) / mat.tile, -np.dot(p - o, v) / mat.tile] for p in pts]
        P, N, UV, T = self._buf(mat)
        base = len(P)
        P.extend(pts.tolist())
        N.extend((np.asarray(normals) if normals is not None else np.tile(n, (len(pts), 1))).tolist())
        UV.extend(np.asarray(uvs).tolist())
        for i in range(1, len(pts) - 1):
            T.append((base, base + i, base + i + 1))

    def quad(self, mat, a, b, c, d, **kw):
        self.poly(mat, [a, b, c, d], **kw)

    def box(self, mat, center, size, yaw=0.0, top=True, bottom=False, top_mat=None):
        """An axis-aligned box rotated by `yaw` (radians) about Y. UVs from world-space meters, so neighbours line up."""
        c = np.asarray(center, np.float64)
        hx, hy, hz = np.asarray(size, np.float64) / 2
        cs, sn = np.cos(yaw), np.sin(yaw)
        ax, az = np.array([cs, 0, -sn]), np.array([sn, 0, cs])
        ay = np.array([0.0, 1.0, 0.0])

        def p(i, j, k):
            return c + ax * hx * i + ay * hy * j + az * hz * k

        faces = [((1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)),        # +x
                 ((-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)),    # -x
                 ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)),        # +z
                 ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1))]    # -z
        for f in faces:
            pts = [p(*v) for v in f]
            self.poly(mat, pts, uv_origin=self._wall_origin(pts))
        if top:
            self.poly(top_mat or mat, [p(-1, 1, 1), p(1, 1, 1), p(1, 1, -1), p(-1, 1, -1)], uv_origin=(0, 0, 0),
                      u_axis=(1, 0, 0))
        if bottom:
            self.poly(mat, [p(-1, -1, -1), p(1, -1, -1), p(1, -1, 1), p(-1, -1, 1)], uv_origin=(0, 0, 0), u_axis=(1, 0, 0))

    @staticmethod
    def _wall_origin(pts):
        # World-anchored texture (UVs measured from the origin), so stone courses line up across pieces.
        return np.zeros(3)

    def segment_wall(self, mat, a, b, y0, y1, thickness, top=True, top_mat=None):
        """A straight wall from a to b (x, z points) between heights y0 and y1."""
        a, b = np.asarray(a, np.float64), np.asarray(b, np.float64)
        d = b - a
        length = np.linalg.norm(d)
        yaw = np.arctan2(-d[1], d[0])
        mid = (a + b) / 2
        self.box(mat, (mid[0], (y0 + y1) / 2, mid[1]), (length, y1 - y0, thickness), yaw, top=top, top_mat=top_mat)

    def cylinder(self, mat, center, radius, y0, y1, segs=20, top_radius=None, cap=False, cap_mat=None, inside=False):
        cx, cz = center
        r1 = radius if top_radius is None else top_radius
        circumference = 2 * np.pi * radius
        P, N, UV, T = self._buf(mat)
        base = len(P)
        slope = (radius - r1) / max(y1 - y0, 1e-6)
        for s in range(segs + 1):
            a = 2 * np.pi * s / segs
            ca, sa = np.cos(a), np.sin(a)
            n = _norm([ca, slope, sa]) * (-1 if inside else 1)
            for y, r in ((y0, radius), (y1, r1)):
                P.append([cx + ca * r, y, cz + sa * r])
                N.append(n.tolist())
                UV.append([s / segs * circumference / mat.tile, -y / mat.tile])
        for s in range(segs):
            i = base + s * 2
            if inside:
                T += [(i, i + 2, i + 1), (i + 1, i + 2, i + 3)]
            else:
                T += [(i, i + 1, i + 2), (i + 1, i + 3, i + 2)]
        if cap:
            m = cap_mat or mat
            ring = [[cx + np.cos(a) * r1, y1, cz + np.sin(a) * r1] for a in np.linspace(2 * np.pi, 0, segs, endpoint=False)]
            self.poly(m, ring, normal=(0, 1, 0), uv_origin=(0, 0, 0), u_axis=(1, 0, 0))

    def cone(self, mat, center, radius, y0, height, segs=20):
        """A conical roof (with a little bell flare at the eaves)."""
        cx, cz = center
        P, N, UV, T = self._buf(mat)
        rings = [(0.0, radius * 1.0), (0.18, radius * 0.78), (1.0, 0.0)]
        heights = [0.0, 0.3, 1.0]
        base = len(P)
        slant = np.hypot(radius, height)
        for s in range(segs + 1):
            a = 2 * np.pi * s / segs
            ca, sa = np.cos(a), np.sin(a)
            for (t, r), hy in zip(rings, heights):
                P.append([cx + ca * r, y0 + hy * height, cz + sa * r])
                n = _norm([ca * height, radius, sa * height])
                N.append(n.tolist())
                UV.append([s / segs * 2 * np.pi * radius / mat.tile * (1 - t * 0.5), -(1 - t) * slant / mat.tile])
        k = len(rings)
        for s in range(segs):
            for j in range(k - 1):
                i = base + s * k + j
                T += [(i, i + 1, i + k), (i + 1, i + k + 1, i + k)]

    def gable_roof(self, mat, center, width, depth, y0, rise, yaw=0.0, overhang=0.6, gable_mat=None):
        """A pitched roof over a width x depth footprint (ridge along depth), with gable end walls."""
        c = np.asarray(center, np.float64)
        cs, sn = np.cos(yaw), np.sin(yaw)
        ax, az = np.array([cs, 0, -sn]), np.array([sn, 0, cs])
        hw, hd = width / 2 + overhang, depth / 2 + overhang
        up = np.array([0, 1.0, 0])

        def p(x, y, z):
            return c + ax * x + up * (y0 + y) + az * z

        eave = -overhang * rise / (width / 2)
        for side in (-1, 1):
            a, b = p(side * hw, eave, -hd), p(side * hw, eave, hd)
            r1, r0 = p(0, rise, hd), p(0, rise, -hd)
            pts = [b, a, r0, r1] if side > 0 else [a, b, r1, r0]
            n = _norm(np.cross(pts[1] - pts[0], pts[2] - pts[0]))
            # u along the ridge, v down the slope
            o = pts[0]
            uax = _norm(pts[1] - pts[0])
            vax = np.cross(n, uax)
            uvs = [[np.dot(q - o, uax) / mat.tile, -np.dot(q - o, vax) / mat.tile] for q in pts]
            self.poly(mat, pts, uvs=uvs)
            # underside, so the eaves aren't see-through from below
            self.poly(mat, pts[::-1], uvs=uvs[::-1])
        g = gable_mat or mat
        hw0 = width / 2
        for zs in (-1, 1):
            tri = [p(-hw0, 0, zs * depth / 2), p(hw0, 0, zs * depth / 2), p(0, rise, zs * depth / 2)]
            if zs < 0:
                tri = tri[::-1]
            self.poly(g, tri)

    def merge(self, other, matrix):
        """Adds another kit's geometry, transformed by a 4x4 matrix (normals by its rotation part)."""
        m = np.asarray(matrix, np.float64)
        r = np.linalg.inv(m[:3, :3]).T
        for name, (P, N, UV, T) in other.parts.items():
            if not T:
                continue
            mat = other.materials[name]
            dst = self._buf(mat)
            base = len(dst[0])
            p = np.asarray(P) @ m[:3, :3].T + m[:3, 3]
            n = np.asarray(N) @ r.T
            n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
            dst[0].extend(p.tolist())
            dst[1].extend(n.tolist())
            dst[2].extend(UV)
            dst[3].extend([(a + base, b + base, c + base) for a, b, c in T])
        return self

    # --- output -----------------------------------------------------------------------------------------------------

    def save(self, rel, textures_rel="../../Textures"):
        """Writes Assets/<rel> (.gltf + .bin) and its .meta. `textures_rel`: from the model's folder to Textures/."""
        images, textures, materials, prims = [], [], [], []

        def texture(uri):
            images.append({"uri": uri})
            textures.append({"source": len(images) - 1})
            return len(textures) - 1

        doc = {"asset": {"version": "2.0", "generator": "Siege meshkit"}, "scene": 0,
               "scenes": [{"nodes": [0]}], "nodes": [{"name": os.path.splitext(os.path.basename(rel))[0], "mesh": 0}]}
        w = G.Writer(doc)
        for name, (P, N, UV, T) in self.parts.items():
            if not T:
                continue
            m = self.materials[name]
            md = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(m.color), "metallicFactor": m.metallic,
                                                         "roughnessFactor": m.roughness}}
            if m.folder:
                stem = os.path.basename(m.folder)
                base = f"{textures_rel}/{m.folder}/{stem}"
                md["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture(f"{base}_diff_2k.jpg")}
                md["normalTexture"] = {"index": texture(f"{base}_nor_gl_2k.jpg")}
                arm = texture(f"{base}_arm_2k.jpg")
                md["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {"index": arm}
                md["occlusionTexture"] = {"index": arm}
            if m.double_sided:
                md["doubleSided"] = True
            if m.emissive:
                md["emissiveFactor"] = list(m.emissive)
            materials.append(md)
            prims.append({"attributes": {"POSITION": w.add(np.asarray(P, np.float32), 34962, minmax=True),
                                         "NORMAL": w.add(np.asarray(N, np.float32), 34962),
                                         "TEXCOORD_0": w.add(np.asarray(UV, np.float32), 34962)},
                          "indices": w.add(np.asarray(T, np.uint32).reshape(-1), 34963), "material": len(materials) - 1})
        doc["images"], doc["textures"], doc["materials"] = images, textures, materials
        doc["meshes"] = [{"name": doc["nodes"][0]["name"], "primitives": prims}]
        path = os.path.join(S.ASSETS, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        w.save(path)
        S.write_meta(rel)
        tris = sum(len(p[3]) for p in self.parts.values())
        print(f"wrote {rel}: {tris} triangles, {len(prims)} materials")
        return S.guid(rel)
