"""The battle's props: the arrow, the catapult (a mangonel: frame and throwing arm as separate models, the arm turned by
the battle script about its axle), and the fire and smoke sprites of the flaming shots (Textures/FX).

Conventions the battle script relies on:
  Arrow.gltf          points along -Z (like a camera), its middle at the origin.
  Catapult_Frame.gltf faces -Z (it throws toward -Z); its axle is at ARM_PIVOT.
  Catapult_Arm.gltf   pivots at its origin, lying along +Z (loaded, bucket at the back); the bucket's cup is at ARM_CUP.
"""
import json
import math
import os

import numpy as np
from PIL import Image, ImageFilter

import meshkit as K
import siege as S

ARM_PIVOT = (0.0, 1.05, 0.9)
ARM_CUP = (0.0, 0.42, 4.15)

WOOD = K.Material("Wood", "Props/rough_wood", tile=1.2)
DARK_WOOD = K.Material("Dark Wood", "Props/rough_wood", tile=1.2, color=(0.55, 0.45, 0.38, 1))
IRON = K.Material("Iron", "Props/rusty_metal_02", tile=0.8, color=(0.45, 0.42, 0.4, 1), metallic=0.7, roughness=0.6)
ROPE = K.Material("Rope", "Props/linen", tile=0.4, color=(0.55, 0.45, 0.3, 1))
SHAFT = K.Material("Arrow Shaft", "Props/rough_wood", tile=0.6, color=(0.85, 0.72, 0.55, 1))
FLETCH = K.Material("Fletching", "Props/linen", tile=0.3, color=(0.92, 0.9, 0.85, 1), double_sided=True)
HEAD = K.Material("Arrowhead", "Props/rusty_metal_02", tile=0.2, color=(0.4, 0.4, 0.42, 1), metallic=0.8, roughness=0.5)


def rod_x(kit, mat, x0, x1, radius, segs=10, y=0.0, z=0.0):
    k = K.Kit()
    k.cylinder(mat, (0.0, 0.0), radius, x0, x1, segs, cap=True)
    # y -> x (the cylinder's axis), x -> -y
    kit.merge(k, K.translate(0, y, z) @ np.array([[0, 1, 0, 0], [-1, 0, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]], float))


def arrow():
    kit = K.Kit()
    # shaft from the tail (+z) to the head (-z); slightly thicker than life so volleys read from afar
    k = K.Kit()
    k.cylinder(SHAFT, (0.0, 0.0), 0.011, -0.42, 0.42, 5)
    kit.merge(k, np.array([[1, 0, 0, 0], [0, 0, -1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], float))  # y -> -z
    k = K.Kit()
    k.cone(HEAD, (0.0, 0.0), 0.022, 0.0, 0.09, 4)
    kit.merge(k, K.translate(0, 0, -0.42) @ np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], float))
    for k3 in range(3):
        a = k3 * 2 * math.pi / 3
        d = np.array([math.cos(a), math.sin(a), 0.0])
        p0, p1 = np.array([0, 0, 0.18]), np.array([0, 0, 0.4])
        kit.poly(FLETCH, [p0 + d * 0.008, p1 + d * 0.008, p1 + d * 0.05, p0 + d * 0.03], uvs=[[0, 0], [1, 0], [1, 1], [0, 1]])
    return kit.save("Models/Props/Arrow.gltf", textures_rel="../../Textures")


def catapult_frame():
    kit = K.Kit()
    # side beams, cross beams
    for x in (-1.05, 1.05):
        kit.box(DARK_WOOD, (x, 0.75, 0.3), (0.3, 0.32, 5.6))
    for z in (-2.2, -0.8, 0.9, 2.6):
        kit.box(DARK_WOOD, (0, 0.75, z), (2.4, 0.26, 0.28))
    # uprights with diagonal braces, the padded crossbar the arm strikes
    for x in (-1.05, 1.05):
        kit.box(WOOD, (x, 2.15, -0.8), (0.28, 2.6, 0.28))
        brace = K.Kit()
        brace.box(WOOD, (0, 0, 0), (0.2, 2.3, 0.2))
        kit.merge(brace, K.translate(x, 1.7, 0.1) @ K.rot_x(38))
        brace = K.Kit()
        brace.box(WOOD, (0, 0, 0), (0.2, 2.0, 0.2))
        kit.merge(brace, K.translate(x, 1.6, -1.7) @ K.rot_x(-35))
    kit.box(WOOD, (0, 3.35, -0.8), (2.5, 0.32, 0.34))
    kit.box(ROPE, (0, 3.35, -0.62), (1.2, 0.36, 0.12))  # padding where the arm strikes
    # axle bearings and the twisted-rope skein around the axle
    for x in (-1.05, 1.05):
        kit.box(IRON, (x, ARM_PIVOT[1], ARM_PIVOT[2]), (0.34, 0.36, 0.36))
    rod_x(kit, ROPE, -0.9, 0.9, 0.2, y=ARM_PIVOT[1], z=ARM_PIVOT[2])
    # wheels
    for x in (-1.32, 1.32):
        for z in (-1.9, 2.3):
            k = K.Kit()
            k.cylinder(DARK_WOOD, (0.0, 0.0), 0.55, -0.1, 0.1, 14, cap=True)
            k.cylinder(IRON, (0.0, 0.0), 0.56, -0.06, 0.06, 14)
            kit.merge(k, K.translate(x, 0.55, z) @ np.array([[0, 1, 0, 0], [-1, 0, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]], float))
    return kit.save("Models/Siege/Catapult_Frame.gltf")


def catapult_arm():
    kit = K.Kit()
    kit.box(WOOD, (0, 0, 1.9), (0.26, 0.3, 4.8))          # the beam, from behind the axle to the bucket
    for z in (0.6, 2.0, 3.2):
        kit.box(IRON, (0, 0, z), (0.3, 0.34, 0.08))      # iron bands
    # the bucket: a shallow box open at the top
    cz = ARM_CUP[2]
    kit.box(DARK_WOOD, (0, 0.2, cz), (0.75, 0.1, 0.75))
    for dx, dz, sx, sz in ((-0.35, 0, 0.06, 0.75), (0.35, 0, 0.06, 0.75), (0, -0.35, 0.75, 0.06), (0, 0.35, 0.75, 0.06)):
        kit.box(DARK_WOOD, (dx, 0.36, cz + dz), (sx, 0.26, sz))
    return kit.save("Models/Siege/Catapult_Arm.gltf")


def soft_sprite(size, seed, kind):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:size, 0:size] / (size - 1) * 2 - 1
    r = np.hypot(x, y)
    noise = np.zeros((size, size))
    for octave in range(5):
        n = rng.random((4 << octave, 4 << octave))
        img = Image.fromarray((n * 255).astype(np.uint8)).resize((size, size), Image.BICUBIC)
        noise += (np.asarray(img) / 255.0 - 0.5) * 0.5 ** octave
    if kind == "smoke":
        a = np.clip(1 - r - noise * 0.6, 0, 1) ** 1.6
        rgb = np.dstack([0.9 + noise * 0.3] * 3)
    else:  # fire: a hot core fading out, ragged edges
        a = np.clip(1 - r * 1.1 - noise * 0.9, 0, 1) ** 1.2
        t = np.clip(1 - r, 0, 1)
        rgb = np.dstack([np.ones_like(t), 0.45 + 0.5 * t, 0.12 + 0.6 * t ** 3])
    out = np.dstack([np.clip(rgb, 0, 1), a])
    return Image.fromarray((out * 255).astype(np.uint8), "RGBA")


def fx_textures():
    folder = os.path.join(S.ASSETS, "Textures", "FX")
    os.makedirs(folder, exist_ok=True)
    for name, kind, seed in (("smoke", "smoke", 4), ("fire", "fire", 9)):
        soft_sprite(256, seed, kind).save(os.path.join(folder, f"{name}.png"))
        S.write_meta(f"Textures/FX/{name}.png")


def main():
    arrow()
    catapult_frame()
    catapult_arm()
    fx_textures()
    with open(os.path.join(os.path.dirname(__file__), "props_layout.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"armPivot": ARM_PIVOT, "armCup": ARM_CUP}, f, indent=1)


if __name__ == "__main__":
    main()
