"""Downloads the Siege sample's third-party assets into Assets/ and writes CREDITS.md.

    python Projects/Siege/Tools/fetch_assets.py [--force]

Every asset here is CC0 (public domain) or CC-BY; CREDITS.md lists each one with its source and license. The files
are committed with the project, so this script only documents (and can repeat) where they came from. Files already
present are skipped unless --force is given.
"""
import json
import os
import re
import sys
import urllib.parse
import urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSETS = os.path.join(ROOT, "Assets")
FORCE = "--force" in sys.argv
AGENT = {"User-Agent": "dimension-siege-assets/1.0"}

# Poly Haven (CC0): PBR textures at 2k (color, OpenGL normal, AO/roughness/metal packed as glTF expects).
TEXTURES = {
    # terrain
    "grass_ground": "Terrain", "aerial_grass_rock": "Terrain", "sparse_grass": "Terrain", "rock_ground": "Terrain",
    "mossy_rock": "Terrain",
    "snow_02": "Terrain", "dirt": "Terrain", "burned_ground_01": "Terrain", "forest_ground_04": "Terrain",
    "rocky_trail": "Terrain",
    # castle
    "castle_wall_slates": "Castle", "castle_brick_07": "Castle", "defense_wall": "Castle",
    "mossy_stone_wall": "Castle", "cobblestone_floor_05": "Castle", "thatch_roof_angled": "Castle",
    "clay_roof_tiles_02": "Castle", "weathered_planks": "Castle",
    # siege engines, weapons and soldiers
    "rough_wood": "Props", "bark_brown_02": "Props", "rusty_metal_02": "Props", "metal_plate": "Props",
    "rough_linen": "Props", "brown_leather": "Props",
}
TEXTURE_MAPS = {"diff": "Diffuse", "nor_gl": "nor_gl", "arm": "arm"}

# Poly Haven (CC0): glTF models at 1k.
MODELS = [
    "rock_face_01", "rock_face_02", "mountainside", "boulder_01", "rock_moss_set_01", "rock_moss_set_02",
    "namaqualand_cliff_02", "rock_07", "rock_09", "stone_01", "fir_sapling", "pine_sapling_small",
    "dead_tree_trunk", "tree_stump_01", "dry_branches_medium_01", "grass_medium_02", "fern_02",
    "kite_shield", "large_castle_door", "wooden_barrels_01", "wooden_crate_01", "wooden_lantern_01",
    "stone_fire_pit", "wooden_bucket_01",
]

# Poly Haven (CC0): sky HDRIs at 2k.
HDRIS = ["kloofendal_48d_partly_cloudy_puresky"]

# Quaternius (CC0), from public mirrors of the itch.io downloads (CC0 allows redistribution).
NAFIS = "https://raw.githubusercontent.com/NafisRayan/Animate-Rigged-Humanoid-No-Blender/HEAD/"
UBC = "Universal Base Characters[Standard]/Universal Base Characters[Standard]/"
UAL = "Universal Animation Library[Standard]/Universal Animation Library[Standard]/"
UAL2 = "Universal Animation Library 2[Standard]/Universal Animation Library 2[Standard]/"
CHARACTER_FILES = {
    "Characters/Superhero_Male_FullBody.gltf": NAFIS + UBC + "Base Characters/Godot - UE/Superhero_Male_FullBody.gltf",
    "Characters/Superhero_Male_FullBody.bin": NAFIS + UBC + "Base Characters/Godot - UE/Superhero_Male_FullBody.bin",
    "Characters/T_Superhero_Male_Dark.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Superhero_Male_Dark.png",
    "Characters/T_Superhero_Male_Ligh.png": NAFIS + UBC + "Base Characters/Textures/T_Superhero_Male_Ligh.png",
    "Characters/T_Superhero_Male_Normal.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Superhero_Male_Normal.png",
    "Characters/T_Superhero_Male_Roughness.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Superhero_Male_Roughness.png",
    "Characters/T_Eye_Brown.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Eye_Brown.png",
    "Characters/T_Eye_Normal.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Eye_Normal.png",
    "Characters/T_Hair_1_BaseColor.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Hair_1_BaseColor.png",
    "Characters/T_Hair_1_Normal.png": NAFIS + UBC + "Base Characters/Godot - UE/T_Hair_1_Normal.png",
    "Characters/Hair_Beard.gltf": NAFIS + UBC + "Hairstyles/Rigged to Head Bone/glTF (Godot -Unreal)/Hair_Beard.gltf",
    "Characters/Hair_Beard.bin": NAFIS + UBC + "Hairstyles/Rigged to Head Bone/glTF (Godot -Unreal)/Hair_Beard.bin",
    "Characters/Hair_Buzzed.gltf": NAFIS + UBC + "Hairstyles/Rigged to Head Bone/glTF (Godot -Unreal)/Hair_Buzzed.gltf",
    "Characters/Hair_Buzzed.bin": NAFIS + UBC + "Hairstyles/Rigged to Head Bone/glTF (Godot -Unreal)/Hair_Buzzed.bin",
    "Characters/License_UniversalBaseCharacters.txt": NAFIS + UBC + "License_Standard.txt",
    "Animations/UAL1_Standard.glb": NAFIS + UAL + "Unreal-Godot/UAL1_Standard.glb",
    "Animations/UAL2_Standard.glb": NAFIS + UAL2 + "Unreal-Godot/UAL2_Standard.glb",
    "Animations/License_UniversalAnimationLibrary.txt": NAFIS + UAL2 + "License.txt",
    "Horse/Horse.gltf": "https://raw.githubusercontent.com/benjaminpjones/catch-the-animal/HEAD/models/glTF/Horse.gltf",
}

CREDITS = """# Credits

The Siege sample's third-party assets. Everything not listed here (the castle, the siege engines, the weapons and
armor, the terrain, the scenes and scripts) was made for the sample. `Tools/fetch_assets.py` downloads these again.

## CC0 1.0 (public domain)

- **Poly Haven** (https://polyhaven.com), CC0: textures {textures}; models {models}; HDRIs {hdris}.
- **Quaternius** (https://quaternius.com), CC0: *Universal Base Characters* (Superhero male body, eyes, hair and
  beard), *Universal Animation Library* and *Universal Animation Library 2* (the soldiers' animations), and the horse
  from the *Ultimate Animated Animal Pack*. Fetched from public CC0 mirrors of the itch.io downloads:
  https://github.com/NafisRayan/Animate-Rigged-Humanoid-No-Blender and
  https://github.com/benjaminpjones/catch-the-animal.
"""


def fetch(url, dest):
    if os.path.exists(dest) and not FORCE:
        return False
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    url = urllib.parse.quote(url, safe=":/%?=&")
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT)) as r, open(dest + ".part", "wb") as f:
        f.write(r.read())
    os.replace(dest + ".part", dest)
    print("  " + os.path.relpath(dest, ROOT))
    return True


def poly_files(asset):
    with urllib.request.urlopen(urllib.request.Request("https://api.polyhaven.com/files/" + asset, headers=AGENT)) as r:
        return json.load(r)


def main():
    print("Poly Haven textures")
    for name, folder in TEXTURES.items():
        files = poly_files(name)
        for suffix, key in TEXTURE_MAPS.items():
            entry = files[key]["2k"].get("jpg") or files[key]["2k"].get("png")
            fetch(entry["url"], os.path.join(ASSETS, "Textures", folder, name, f"{name}_{suffix}_2k.jpg"))
    print("Poly Haven models")
    for name in MODELS:
        gltf = poly_files(name)["gltf"]["1k"]["gltf"]
        base = os.path.join(ASSETS, "Models", "Nature" if name not in
                            ("kite_shield", "large_castle_door", "wooden_barrels_01", "wooden_crate_01",
                             "wooden_lantern_01", "stone_fire_pit", "wooden_bucket_01") else "Props", name)
        fetch(gltf["url"], os.path.join(base, f"{name}.gltf"))
        for rel, inc in gltf.get("include", {}).items():
            fetch(inc["url"], os.path.join(base, rel))
    print("Poly Haven HDRIs")
    for name in HDRIS:
        fetch(poly_files(name)["hdri"]["2k"]["hdr"]["url"], os.path.join(ASSETS, "Sky", f"{name}_2k.hdr"))
    print("Quaternius")
    for rel, url in CHARACTER_FILES.items():
        fetch(url, os.path.join(ASSETS, "Models", rel))
    # The body's glTF names some textures "<name>_png.png" (Blender's export of the "<name>.png" images): the
    # mirror only has "<name>.png".
    body = os.path.join(ASSETS, "Models", "Characters", "Superhero_Male_FullBody.gltf")
    with open(body, encoding="utf-8") as f:
        text = f.read()
    with open(body, "w", encoding="utf-8", newline="") as f:
        f.write(re.sub(r'"uri":"([^"]+)_png\.png"', r'"uri":"\1.png"', text))
    with open(os.path.join(ROOT, "CREDITS.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(CREDITS.format(textures=", ".join(f"`{t}`" for t in TEXTURES),
                               models=", ".join(f"`{m}`" for m in MODELS),
                               hdris=", ".join(f"`{h}`" for h in HDRIS)))
    print("Wrote CREDITS.md")


if __name__ == "__main__":
    main()
