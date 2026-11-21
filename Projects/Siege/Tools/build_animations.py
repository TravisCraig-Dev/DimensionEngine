"""Models/Animations/Soldier_Anims.gltf: one animation source for every soldier. An Animator plays clips from a single
model, and the battle needs clips from both of Quaternius' Universal Animation Libraries (same skeleton), plus clips
authored here (see authored_clips). The file holds the skeleton (UAL1's nodes) and the animations, channels matched to
joints by name."""
import os

import numpy as np

import gltfio as G
import siege as S

SRC = os.path.join(S.ASSETS, "Models", "Animations")
OUT_REL = "Models/Animations/Soldier_Anims.gltf"

UAL1_CLIPS = ["Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop", "Sword_Attack", "Sword_Idle", "Death01",
              "Hit_Chest", "Hit_Head", "Spell_Simple_Shoot", "Pistol_Aim_Neutral", "Sitting_Idle_Loop", "Driving_Loop",
              "Crouch_Idle_Loop", "Roll", "Punch_Cross"]
UAL2_CLIPS = ["Sword_Regular_A", "Sword_Regular_B", "Sword_Regular_C", "Sword_Regular_Combo", "Sword_Heavy_Combo",
              "Sword_Block", "Sword_Dash", "Idle_Shield_Loop", "Idle_Shield_Break", "Shield_Dash", "Shield_OneShot",
              "Hit_Knockback", "OverhandThrow", "Melee_Hook", "LayToIdle"]


def copy_skeleton(src_doc):
    nodes = []
    for n in src_doc["nodes"]:
        m = {k: v for k, v in n.items() if k in ("name", "children", "translation", "rotation", "scale", "matrix")}
        nodes.append(m)
    scene = src_doc["scenes"][src_doc.get("scene", 0)]
    return nodes, list(scene["nodes"])


def main(authored=None):
    ual1 = G.Gltf(os.path.join(SRC, "UAL1_Standard.glb"))
    ual2 = G.Gltf(os.path.join(SRC, "UAL2_Standard.glb"))
    nodes, roots = copy_skeleton(ual1.doc)
    index = {n.get("name"): i for i, n in enumerate(nodes)}
    doc = {"asset": {"version": "2.0", "generator": "Siege build_animations"}, "scene": 0,
           "scenes": [{"nodes": roots}], "nodes": nodes, "animations": []}
    w = G.Writer(doc)
    # The engine reads animations from skinned files: UAL1's skin (all joints, their bind matrices) on a single
    # degenerate triangle, so the file is a valid animation source without carrying a body.
    skin = ual1.doc["skins"][0]
    ibm = ual1.read(skin["inverseBindMatrices"]).astype(np.float32)
    doc["skins"] = [{"joints": [index[ual1.doc["nodes"][j]["name"]] for j in skin["joints"]],
                     "inverseBindMatrices": w.add(ibm)}]
    tri = np.zeros((3, 3), np.float32)
    tri[1, 0] = tri[2, 2] = 0.001
    doc["meshes"] = [{"name": "Skeleton", "primitives": [{"attributes": {
        "POSITION": w.add(tri, 34962, minmax=True), "NORMAL": w.add(np.tile(np.float32([0, 1, 0]), (3, 1)), 34962),
        "JOINTS_0": w.add(np.zeros((3, 4), np.uint16), 34962),
        "WEIGHTS_0": w.add(np.tile(np.float32([1, 0, 0, 0]), (3, 1)), 34962)},
        "indices": w.add(np.array([0, 1, 2], np.uint32), 34963)}]}]
    nodes.append({"name": "Skeleton", "mesh": 0, "skin": 0})
    doc["scenes"][0]["nodes"].append(len(nodes) - 1)
    for src, names in ((ual1, UAL1_CLIPS), (ual2, UAL2_CLIPS)):
        by_name = {a["name"]: a for a in src.doc["animations"]}
        for name in names:
            a = by_name[name]
            out = {"name": name, "channels": [], "samplers": []}
            for ch in a["channels"]:
                node_name = src.doc["nodes"][ch["target"]["node"]].get("name")
                if node_name not in index:
                    continue
                s = a["samplers"][ch["sampler"]]
                times = src.read(s["input"]).astype(np.float32).reshape(-1)
                values = src.read(s["output"]).astype(np.float32)
                out["samplers"].append({"input": w.add(times, minmax=True), "output": w.add(values),
                                        "interpolation": s.get("interpolation", "LINEAR")})
                out["channels"].append({"sampler": len(out["samplers"]) - 1,
                                        "target": {"node": index[node_name], "path": ch["target"]["path"]}})
            doc["animations"].append(out)
    for clip in (authored or []):
        # clip: {"name", "tracks": {joint name: {"rotation": (times, quats xyzw), "translation": ...}}}
        out = {"name": clip["name"], "channels": [], "samplers": []}
        for joint, paths in clip["tracks"].items():
            for path, (times, values) in paths.items():
                out["samplers"].append({"input": w.add(np.asarray(times, np.float32), minmax=True),
                                        "output": w.add(np.asarray(values, np.float32)), "interpolation": "LINEAR"})
                out["channels"].append({"sampler": len(out["samplers"]) - 1,
                                        "target": {"node": index[joint], "path": path}})
        doc["animations"].append(out)
    w.save(os.path.join(S.ASSETS, OUT_REL))
    S.write_meta(OUT_REL)
    print(f"wrote {OUT_REL}: {len(doc['animations'])} clips")


if __name__ == "__main__":
    main()
