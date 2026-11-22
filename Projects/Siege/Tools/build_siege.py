"""Scenes/Siege.scene: the valley (two terrains), sky, sun and the hero camera. Later stages add the castle, the
armies and the siege engines (see the other build_*.py tools, run from here)."""
import json
import math
import os
import sys

import numpy as np

import siege as S
import terrain_gen as T

INNER_SIZE, INNER_RES, INNER_HEIGHT = 1400.0, 1024, 400.0
OUTER_SIZE, OUTER_RES, OUTER_HEIGHT = 9000.0, 1024, 2000.0


def tex(folder, name, suffix):
    return S.write_meta(f"Textures/{folder}/{name}/{name}_{suffix}_2k.jpg")


def layer(name, folder, tex_name, tiling, color=(1, 1, 1, 1), normal=1.0, triplanar=False, trees=0.0, grass=0.0):
    return {"Name": name, "Texture": tex(folder, tex_name, "diff"), "Color": list(color), "Tiling": tiling,
            "Normal Map": tex(folder, tex_name, "nor_gl"), "Normal Strength": normal, "Triplanar": triplanar,
            "Trees": trees, "Grass": grass}


def inner_weights(x, z, h, road):
    spacing = INNER_SIZE / (INNER_RES - 1)
    slope = T.slope_degrees(h, spacing)
    n1 = T.fbm(x, z, 1 / 60, 3, offset=21) * 0.5 + 0.5
    n2 = T.fbm(x, z, 1 / 25, 3, offset=33) * 0.5 + 0.5
    cliff = T.smoothstep(46, 56, slope)
    rock = T.smoothstep(36, 46, slope + (n2 - 0.5) * 14) * (1 - cliff)
    open_ground = 1 - cliff - rock
    # Forest on the mountain's lower slopes and in patches on the plains' edges; not on the battlefield or road.
    d_battle = np.hypot(x - T.BATTLE_CENTER[0], (z - T.BATTLE_CENTER[1]) * 0.8)
    battlefield = T.smoothstep(430, 300, d_battle)
    d_castle = np.hypot(x - T.CASTLE_CENTER[0], z - T.CASTLE_CENTER[1])
    forest = T.smoothstep(0.52, 0.62, n1) * (1 - battlefield) * T.smoothstep(T.CASTLE_RADIUS + 40, T.CASTLE_RADIUS + 90, d_castle)
    forest *= T.smoothstep(130, 40, h - T.PLAINS_HEIGHT) * T.smoothstep(2, 14, h - T.PLAINS_HEIGHT * 0.5)
    forest *= T.smoothstep(12, 30, road)
    trampled = battlefield * T.smoothstep(0.55, 0.8, n2) * 0.35
    trail = T.smoothstep(7, 3.5, road)
    sparse = T.smoothstep(0.3, 0.8, n1) * 0.45 + T.smoothstep(40, 120, h - T.PLAINS_HEIGHT) * 0.5
    w = np.zeros((8,) + h.shape)
    w[1] = rock
    w[2] = cliff
    rest = open_ground * (1 - trail)
    w[4] = rest * forest
    rest2 = rest * (1 - forest)
    w[3] = rest2 * trampled
    w[6] = rest2 * (1 - trampled) * np.clip(sparse, 0, 1)
    w[0] = rest2 * (1 - trampled) * (1 - np.clip(sparse, 0, 1))
    w[5] = open_ground * trail + (cliff + rock) * trail * 0.3
    # The castle plateau: rock and trodden earth, no grass.
    plateau = T.smoothstep(T.CASTLE_RADIUS + 5, T.CASTLE_RADIUS - 10, d_castle)
    w = w * (1 - plateau) + plateau * np.stack([np.zeros_like(h)] * 1 + [n1 * 0.6] + [np.zeros_like(h)] + [0.4 + 0 * h] +
                                                [np.zeros_like(h)] * 4)
    return w


def outer_heights():
    x, z, h, road = T.heights(OUTER_SIZE, OUTER_RES, erode=False)
    # Inside the inner terrain's square, sink well below it (the inner one draws there); at its edge, match.
    half = INNER_SIZE / 2
    edge = np.maximum(np.abs(x), np.abs(z))
    sink = T.smoothstep(half - 8, half - 60, edge) * 25 + T.smoothstep(half + 40, half - 8, edge) * 0.6
    return x, z, h - sink


def outer_weights(x, z, h):
    spacing = OUTER_SIZE / (OUTER_RES - 1)
    slope = T.slope_degrees(h, spacing)
    n1 = T.fbm(x, z, 1 / 300, 3, offset=41) * 0.5 + 0.5
    snow = T.smoothstep(720, 900, h + n1 * 120) * (1 - T.smoothstep(48, 60, slope) * 0.6)
    cliff = T.smoothstep(38, 50, slope) * (1 - snow)
    rock = T.smoothstep(28, 38, slope) * (1 - cliff - snow)
    open_ground = np.clip(1 - snow - cliff - rock, 0, 1)
    forest = T.smoothstep(0.45, 0.6, n1) * T.smoothstep(650, 450, h)
    w = np.zeros((5,) + h.shape)
    w[0] = open_ground * (1 - forest)
    w[1] = rock
    w[2] = cliff
    w[3] = snow
    w[4] = open_ground * forest
    return w


# The hero camera films the battle (Scripts/CinematicCamera.cs).
CINEMATIC = {"Scripts": [{"Class": "Siege.CinematicCamera", "Fields": {"AutoPlay": True, "FirstShot": 0, "Pace": 1.0,
                                                                     "Handheld": 0.3, "CatapultInterrupts": True,
                                                                     "CatapultEvery": 45.0}, "Enabled": True}]}


def look(position, target):
    d = np.array(target, float) - np.array(position, float)
    yaw = math.atan2(-d[0], -d[2])
    pitch = math.atan2(d[1], math.hypot(d[0], d[2]))
    return [pitch, yaw, 0.0]


def main():
    S.meta_all()
    sc = S.Scene("Siege", 100_000)
    sc.settings = {
        "ambientColor": [0.55, 0.6, 0.7], "ambientIntensity": 0.3, "bloom": True, "bloomIntensity": 0.35,
        "bloomThreshold": 1.6, "exposure": 0.85, "fxaa": False, "taa": True, "ssao": True, "ssaoIntensity": 1.0,
        "ssaoRadius": 0.8, "tonemapping": True, "skyMode": 1, "skyIntensity": 1.0, "skyRotation": 2.2,
        "skyTexture": S.guid("Sky/kloofendal_48d_partly_cloudy_puresky_2k.hdr"), "vignette": 0.22, "gravity": [0, -9.81, 0],
        "fog": True, "fogDensity": 0.00022, "fogHeight": 20.0, "fogHeightFalloff": 0.0025, "fogSunScattering": 0.35,
        "fogIntensity": 0.5, "fogMaxDistance": 6000, "fogAnisotropy": 0.65, "wind": [2.2, 0.0, 1.2], "windGusts": 0.5,
    }
    hero = (-150.0, 52.0, 300.0)
    target = (25.0, 190.0, -430.0)
    # (python build_siege.py --camera x,y,z --target x,y,z: another view, for checks)
    args = sys.argv[1:]
    if "--camera" in args:
        hero = tuple(float(v) for v in args[args.index("--camera") + 1].split(","))
    if "--target" in args:
        target = tuple(float(v) for v in args[args.index("--target") + 1].split(","))
    sc.add("Main Camera", hero, look(hero, target), Camera={
        "Projection": 0, "Field Of View": 34, "Orthographic Size": 5, "Near Clip": 0.3, "Far Clip": 12000,
        "Primary": True, "Background": [0.4, 0.5, 0.6, 1], "Clear Flags": 1},
        Script=CINEMATIC)
    sc.add("Sun", (0, 500, 0), (-0.42, -2.25, 0), Directional_Light={
        "Color": [1.0, 0.93, 0.82], "Intensity": 3.6, "Soft Shadows": True, "Light Angle": 1.0})

    # The inner terrain: the castle mountain, the road and the battlefield.
    x, z, h, road = T.heights(INNER_SIZE, INNER_RES)
    np.save(os.path.join(os.path.dirname(__file__), "cache_inner_heights.npy"), h.astype(np.float32))
    import build_castle
    castle = build_castle.build()
    layers = [
        layer("Grass", "Terrain", "grass_ground", 0.2, (0.66, 0.9, 0.5, 1), grass=1.0),
        layer("Rock", "Terrain", "rock_ground", 0.12, normal=1.3, triplanar=True),
        layer("Cliff", "Terrain", "mossy_rock", 0.1, normal=1.5, triplanar=True),
        layer("Trampled Earth", "Terrain", "dirt", 0.2, grass=0.15),
        layer("Forest Floor", "Terrain", "forest_ground_04", 0.2, trees=1.0, grass=0.25),
        layer("Road", "Terrain", "rocky_trail", 0.2),
        layer("Sparse Grass", "Terrain", "sparse_grass", 0.2, grass=0.45),
        layer("Scorched", "Terrain", "burned_ground_01", 0.2),
    ]
    sc.add("Valley", Mesh_Renderer={"Mesh": 3, "Albedo": 0, "Color": [1, 1, 1, 1], "Tiling": [1, 1], "Smoothness": 0.05,
                                    "Emission": 0.0},
           Terrain={"Size": [INNER_SIZE, INNER_SIZE], "Height": INNER_HEIGHT, "Resolution": INNER_RES,
                    "Layers": layers, "Height Blend": 0.35,
                    "Tree Mesh": S.guid("Models/Trees/Fir_B.gltf"), "Tree Density": 2.2, "Tree Scale": [0.7, 1.35],
                    "Tree Max Slope": 34,
                    # The grass: a dense field made on the GPU around the camera (Tools/build_grass.py's tuft).
                    "Grass Mesh": S.guid("Models/Nature/Grass_Tuft.gltf"), "Grass Density": 600.0, "Grass Scale": [0.8, 1.3],
                    "Grass Max Slope": 32.0, "Grass Sway": 1.0, "Grass Bending": 1.0, "Grass Distance": 55.0,
                    "Sculpted Heights": T.encode_heights(h, INNER_HEIGHT),
                    "Painted Layers": T.encode_layers(inner_weights(x, z, h, road))})

    # The outer terrain: the ranges around the valley.
    xo, zo, ho = outer_heights()
    outer_layers = [
        layer("Meadow", "Terrain", "aerial_grass_rock", 0.05, (0.62, 0.8, 0.5, 1)),
        layer("Rock", "Terrain", "rock_ground", 0.06, triplanar=True),
        layer("Cliff", "Terrain", "mossy_rock", 0.06, normal=1.3, triplanar=True),
        layer("Snow", "Terrain", "snow_02", 0.04, (0.95, 0.97, 1.0, 1)),
        layer("Forest", "Terrain", "forest_ground_04", 0.05, (0.55, 0.65, 0.5, 1), trees=1.0),
    ]
    sc.add("Mountains", (0, 0, 0), Mesh_Renderer={"Mesh": 3, "Albedo": 0, "Color": [1, 1, 1, 1], "Tiling": [1, 1],
                                                   "Smoothness": 0.05, "Emission": 0.0},
           Terrain={"Size": [OUTER_SIZE, OUTER_SIZE], "Height": OUTER_HEIGHT, "Resolution": OUTER_RES,
                    "Layers": outer_layers, "Height Blend": 0.5,
                    "Tree Mesh": S.guid("Models/Trees/Fir_A.gltf"), "Tree Density": 0.8, "Tree Scale": [1.0, 1.6],
                    "Tree Max Slope": 32,
                    "Sculpted Heights": T.encode_heights(np.clip(ho, 0, None), OUTER_HEIGHT),
                    "Painted Layers": T.encode_layers(outer_weights(xo, zo, ho))})
    # The castle (its model's origin is the crag's center at height 0) and its gate.
    sc.add("Castle", (float(T.CASTLE_CENTER[0]), 0.0, float(T.CASTLE_CENTER[1])),
           Mesh_Renderer=S.mesh_renderer(castle["model"]))
    # The battle (Scripts/Battle.cs) and the objects it copies.
    import battle_templates
    battle_templates.add(sc)
    guid = sc.save("Scenes/Siege.scene")
    with open(os.path.join(S.ROOT, "Project.dproj"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"name": "Siege", "gameModule": "", "windowWidth": 1600, "windowHeight": 900,
                   "engineVersion": "0.1.0", "startScene": guid, "buildScenes": [guid]}, f, indent=2)
    print("Siege.scene written")


if __name__ == "__main__":
    main()
