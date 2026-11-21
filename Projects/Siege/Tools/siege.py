"""Shared helpers for the Siege sample's generators: stable asset GUIDs (.meta files), materials and scene JSON."""
import hashlib
import json
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSETS = os.path.join(ROOT, "Assets")

_TYPES = {
    ".png": "Texture", ".jpg": "Texture", ".jpeg": "Texture", ".hdr": "Texture",
    ".gltf": "Mesh", ".glb": "Mesh", ".obj": "Mesh", ".fbx": "Mesh", ".dmodel": "Mesh",
    ".scene": "Scene", ".prefab": "Prefab", ".mat": "Material", ".controller": "AnimatorController",
    ".anim": "AnimationClip", ".vfx": "VisualEffect", ".wav": "Audio", ".ogg": "Audio", ".timeline": "Timeline",
}
_LINEAR_HINTS = ("_nor", "_arm", "_rough", "_ao", "_disp", "normal", "roughness", "_mask")


def guid(rel):
    """A stable GUID for an asset path (relative to Assets/, forward slashes): the same path, the same GUID."""
    h = int(hashlib.md5(rel.replace("\\", "/").lower().encode("utf-8")).hexdigest()[:15], 16)
    return 1_000_000 + h  # (clear of the built-in handles and small hand-made ids)


def write_meta(rel, settings=None):
    """Writes <asset>.meta (keeping an existing one's GUID) and returns the asset's GUID."""
    path = os.path.join(ASSETS, rel)
    meta_path = path + ".meta"
    ext = os.path.splitext(rel)[1].lower()
    kind = _TYPES.get(ext)
    if kind is None:
        return None
    g = guid(rel)
    if os.path.exists(meta_path):
        try:
            with open(meta_path, encoding="utf-8") as f:
                g = json.load(f)["guid"]
        except (ValueError, KeyError):
            pass
    if settings is None:
        settings = {}
        if kind == "Texture":
            name = os.path.basename(rel).lower()
            linear = any(h in name for h in _LINEAR_HINTS)
            settings = {"filter": 0, "srgb": not linear and ext != ".hdr", "mips": ext != ".hdr"}
    with open(meta_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"guid": g, "type": kind, "settings": settings}, f, indent=2)
    return g


def meta_all():
    """Writes a .meta for every importable file under Assets/ that has none. Returns {relative path: guid}."""
    out = {}
    for dirpath, _, files in os.walk(ASSETS):
        for name in files:
            if name.endswith(".meta"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), ASSETS).replace("\\", "/")
            g = write_meta(rel) if not os.path.exists(os.path.join(dirpath, name + ".meta")) else \
                json.load(open(os.path.join(dirpath, name + ".meta"), encoding="utf-8"))["guid"]
            if g:
                out[rel] = g
    return out


def write_json(rel, data):
    path = os.path.join(ASSETS, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
    return write_meta(rel)


def pbr_material(rel, tex_folder, tiling=(1, 1), color=(1, 1, 1, 1), normal_strength=1.0, metallic=0.0):
    """A material from a Poly Haven texture folder (…_diff_2k / _nor_gl_2k / _arm_2k)."""
    name = os.path.basename(tex_folder)
    base = f"{tex_folder}/{name}"
    return write_json(rel, {
        "Base Color": list(color), "Base Map": write_meta(base + "_diff_2k.jpg"),
        "Normal Map": write_meta(base + "_nor_gl_2k.jpg"), "Normal Strength": normal_strength,
        "Metallic": metallic, "Roughness": 1.0, "Metallic Roughness Map": write_meta(base + "_arm_2k.jpg"),
        "Occlusion Map": write_meta(base + "_arm_2k.jpg"), "Occlusion Strength": 1.0,
        "Emission Color": [0, 0, 0], "Emission Strength": 1.0, "Emission Map": 0,
        "Tiling": list(tiling), "Offset": [0, 0], "Blend Mode": 0, "Alpha Cutoff": 0.5,
        "Double Sided": False, "Cast Shadows": True,
    })


class Scene:
    """Builds a .scene file: entities with ids from `start` upward."""

    def __init__(self, name, start):
        self.name = name
        self.next = start
        self.entities = []
        self.settings = {}

    def add(self, name, position=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), parent=0, active=True, **components):
        g = self.next
        self.next += 1
        comps = {"Tag": {"Name": name, "Tag": "Untagged", "Active": active, "Layer": 0},
                 "Transform": {"Position": list(position), "Rotation": list(rotation), "Scale": list(scale)}}
        for key, value in components.items():
            comps[key.replace("_", " ")] = value
        self.entities.append({"guid": g, "parent": parent, "components": comps})
        return g

    def save(self, rel):
        return write_json(rel, {"version": 1, "name": self.name, "settings": self.settings, "entities": self.entities})


def mesh_renderer(mesh, material=0, color=(1, 1, 1, 1), shadows=True):
    return {"Mesh": mesh, "Material": material, "Albedo": 0, "Color": list(color), "Tiling": [1, 1],
            "Smoothness": 0.5, "Emission": 0.0, "Cast Shadows": shadows}


def animator(clip, source=0, speed=1.0, loop=True, controller=0):
    # Retargeting by joint name: every soldier has the clips' own skeleton, so they play exactly as made (the
    # humanoid mapping's muscle limits bent the authored bow draw, and it drops joints such as the bowstring).
    return {"Clip": clip, "Speed": speed, "Loop": loop, "Playing": True, "Controller": controller,
            "Animation Source": source, "Retargeting": 1, "Muscle Limits": True, "Foot IK": False, "Foot IK Reach": 0.5}
