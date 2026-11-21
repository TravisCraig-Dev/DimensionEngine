"""Procedural textures the scans don't cover: riveted chainmail (color, OpenGL normal, AO/roughness/metal)."""
import os

import numpy as np
from PIL import Image

import siege as S


def chainmail(size=1024, rings_across=24):
    """Interlocking rings in offset rows: a height field of tori, each ring tilted so neighbours overlap."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float64) / size
    cell = 1.0 / rings_across
    height = np.zeros((size, size))
    occlusion = np.ones((size, size))
    radius, wire = cell * 0.44, cell * 0.1
    for row in range(-1, rings_across * 2 + 1):
        cy = row * cell * 0.5
        shift = (row % 2) * cell * 0.5
        for col in range(-1, rings_across + 1):
            cx = col * cell + shift
            # Wrap so the texture tiles.
            dx = (x - cx + 0.5) % 1.0 - 0.5
            dy = (y - cy + 0.5) % 1.0 - 0.5
            d = np.sqrt(dx * dx + (dy * 1.15) ** 2)
            t = np.clip(1 - ((d - radius) / wire) ** 2, 0, None)
            ring = np.sqrt(t) * (0.7 + 0.3 * np.clip(dy / radius + 0.5, 0, 1))  # tilted: higher at the bottom
            height = np.maximum(height, ring)
    occlusion = np.clip(0.35 + height * 0.9, 0, 1)
    gy, gx = np.gradient(height)
    strength = size / rings_across * 0.35
    n = np.dstack([-gx * strength, gy * strength, np.ones_like(height)])  # OpenGL: green up
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    normal = ((n * 0.5 + 0.5) * 255).astype(np.uint8)
    rng = np.random.default_rng(7)
    grime = np.clip(rng.normal(0, 0.04, (size // 8, size // 8)), -0.1, 0.1)
    grime = np.array(Image.fromarray(((grime + 0.1) * 1275).astype(np.uint8)).resize((size, size), Image.BICUBIC)) / 255.0
    metal_mask = height > 0.05
    base = np.where(metal_mask, 0.52 + grime * 0.3, 0.06)
    color = np.dstack([base * 0.97, base * 0.96, base * 0.95])
    color = (np.clip(color, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    rough = np.where(metal_mask, 0.38 + grime * 0.5, 0.9)
    metal = np.where(metal_mask, 1.0, 0.0)
    arm = (np.dstack([occlusion, rough, metal]) * 255).astype(np.uint8)
    return color, normal, arm


def neutral_linen():
    """Poly Haven's rough_linen is dyed light blue: the same weave undyed (its luminance, a warm off-white), so
    garment colors multiply onto it as they should. Its normal and ARM maps are reused as they are."""
    src = os.path.join(S.ASSETS, "Textures", "Props", "rough_linen")
    dst = os.path.join(S.ASSETS, "Textures", "Props", "linen")
    os.makedirs(dst, exist_ok=True)
    a = np.asarray(Image.open(os.path.join(src, "rough_linen_diff_2k.jpg")).convert("RGB"), dtype=np.float64) / 255
    lum = a @ np.array([0.2126, 0.7152, 0.0722])
    lum = lum / lum.mean() * 0.78
    out = np.dstack([lum * 1.0, lum * 0.96, lum * 0.88])
    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(os.path.join(dst, "linen_diff_2k.jpg"), quality=92)
    for suffix in ("nor_gl", "arm"):
        Image.open(os.path.join(src, f"rough_linen_{suffix}_2k.jpg")).save(os.path.join(dst, f"linen_{suffix}_2k.jpg"), quality=95)
    for suffix in ("diff", "nor_gl", "arm"):
        S.write_meta(f"Textures/Props/linen/linen_{suffix}_2k.jpg")


def slate_tiles():
    """Grey-blue slate roofing from Poly Haven's clay_roof_tiles_02 (terracotta): its luminance with the contrast
    raised a little, so tower cones read as slate. Normal and ARM maps reused."""
    src = os.path.join(S.ASSETS, "Textures", "Castle", "clay_roof_tiles_02")
    dst = os.path.join(S.ASSETS, "Textures", "Castle", "slate_tiles")
    os.makedirs(dst, exist_ok=True)
    a = np.asarray(Image.open(os.path.join(src, "clay_roof_tiles_02_diff_2k.jpg")).convert("RGB"), dtype=np.float64) / 255
    lum = a @ np.array([0.2126, 0.7152, 0.0722])
    lum = (lum - lum.mean()) * 1.4 + 0.3
    out = np.dstack([lum * 0.92, lum * 0.97, lum * 1.05])
    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(os.path.join(dst, "slate_tiles_diff_2k.jpg"), quality=92)
    for suffix in ("nor_gl", "arm"):
        Image.open(os.path.join(src, f"clay_roof_tiles_02_{suffix}_2k.jpg")).save(os.path.join(dst, f"slate_tiles_{suffix}_2k.jpg"), quality=95)
    for suffix in ("diff", "nor_gl", "arm"):
        S.write_meta(f"Textures/Castle/slate_tiles/slate_tiles_{suffix}_2k.jpg")


def main():
    neutral_linen()
    slate_tiles()
    folder = os.path.join(S.ASSETS, "Textures", "Props", "chainmail")
    os.makedirs(folder, exist_ok=True)
    color, normal, arm = chainmail()
    for suffix, img in (("diff", color), ("nor_gl", normal), ("arm", arm)):
        path = os.path.join(folder, f"chainmail_{suffix}_2k.jpg")
        Image.fromarray(img).save(path, quality=92)
        S.write_meta(os.path.relpath(path, S.ASSETS).replace("\\", "/"))
    print("chainmail written")


if __name__ == "__main__":
    main()
