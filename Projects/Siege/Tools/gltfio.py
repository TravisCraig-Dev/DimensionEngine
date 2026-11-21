"""Minimal glTF 2.0 reading and writing with numpy, for the Siege sample's model generators."""
import base64
import json
import os
import struct

import numpy as np

_COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


class Gltf:
    """A loaded .gltf/.glb: the JSON in `doc`, accessors read with `read(index)`."""

    def __init__(self, path):
        self.path = path
        self.dir = os.path.dirname(path)
        data = open(path, "rb").read()
        self.blobs = {}
        if data[:4] == b"glTF":
            n = struct.unpack("<I", data[12:16])[0]
            self.doc = json.loads(data[20:20 + n])
            off = 20 + n
            if off < len(data):
                bn = struct.unpack("<I", data[off:off + 4])[0]
                self.blobs[0] = data[off + 8:off + 8 + bn]
        else:
            self.doc = json.loads(data.decode("utf-8"))
        for i, b in enumerate(self.doc.get("buffers", [])):
            if i in self.blobs:
                continue
            uri = b["uri"]
            if uri.startswith("data:"):
                self.blobs[i] = base64.b64decode(uri.split(",", 1)[1])
            else:
                self.blobs[i] = open(os.path.join(self.dir, uri), "rb").read()

    def read(self, index):
        a = self.doc["accessors"][index]
        dt = _COMP[a["componentType"]]
        n = _NCOMP[a["type"]]
        count = a["count"]
        if "bufferView" not in a:
            return np.zeros((count, n) if n > 1 else count, dtype=dt)
        v = self.doc["bufferViews"][a["bufferView"]]
        blob = self.blobs[v["buffer"]]
        start = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        item = np.dtype(dt).itemsize * n
        stride = v.get("byteStride", item)
        if stride == item:
            arr = np.frombuffer(blob, dtype=dt, count=count * n, offset=start).copy()
        else:
            arr = np.empty(count * n, dtype=dt)
            for i in range(count):
                arr[i * n:(i + 1) * n] = np.frombuffer(blob, dtype=dt, count=n, offset=start + i * stride)
        if a.get("normalized") and dt != np.float32:
            arr = arr.astype(np.float32) / np.iinfo(dt).max
        return arr.reshape(count, n) if n > 1 else arr

    def node_by_name(self, name):
        for i, n in enumerate(self.doc["nodes"]):
            if n.get("name") == name:
                return i
        return None


def node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
    t = np.array(node.get("translation", [0, 0, 0]), dtype=np.float64)
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * s
    m[:3, 3] = t
    return m


def world_matrices(doc):
    """Each node's world matrix (4x4, column vectors)."""
    parent = {}
    for i, n in enumerate(doc["nodes"]):
        for c in n.get("children", []):
            parent[c] = i
    cache = {}

    def world(i):
        if i not in cache:
            m = node_matrix(doc["nodes"][i])
            cache[i] = world(parent[i]) @ m if i in parent else m
        return cache[i]

    return [world(i) for i in range(len(doc["nodes"]))]


class Writer:
    """Builds a .gltf with one external .bin: add accessors, then save."""

    def __init__(self, doc):
        self.doc = doc
        self.doc.setdefault("asset", {"version": "2.0", "generator": "Dimension Siege tools"})
        self.doc["buffers"] = []
        self.doc["bufferViews"] = []
        self.doc["accessors"] = []
        self.bin = bytearray()

    def add(self, array, target=None, minmax=False, normalized=False):
        array = np.ascontiguousarray(array)
        comp = {np.dtype(np.float32): 5126, np.dtype(np.uint32): 5125, np.dtype(np.uint16): 5123,
                np.dtype(np.uint8): 5121}[array.dtype]
        n = 1 if array.ndim == 1 else array.shape[1]
        kind = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}[n]
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": array.nbytes}
        if target:
            view["target"] = target
        self.bin += array.tobytes()
        self.doc["bufferViews"].append(view)
        acc = {"bufferView": len(self.doc["bufferViews"]) - 1, "componentType": comp, "count": int(array.shape[0]),
               "type": kind}
        if normalized:
            acc["normalized"] = True
        if minmax:
            flat = array.reshape(array.shape[0], -1)
            acc["min"] = flat.min(axis=0).tolist()
            acc["max"] = flat.max(axis=0).tolist()
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def save(self, path):
        name = os.path.splitext(os.path.basename(path))[0] + ".bin"
        self.doc["buffers"] = [{"uri": name, "byteLength": len(self.bin)}]
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(os.path.join(os.path.dirname(path), name), "wb") as f:
            f.write(self.bin)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.doc, f, indent=1)


def vertex_normals(pos, tri):
    """Area-weighted smooth normals."""
    n = np.zeros_like(pos, dtype=np.float64)
    a, b, c = pos[tri[:, 0]], pos[tri[:, 1]], pos[tri[:, 2]]
    fn = np.cross(b - a, c - a)
    for k in range(3):
        np.add.at(n, tri[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    return (n / np.maximum(ln, 1e-12)).astype(np.float32)
