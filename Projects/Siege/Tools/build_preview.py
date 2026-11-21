"""Scenes/AssetPreview.scene: the third-party assets side by side, to check how they import and look."""
import json
import os
import siege as S

S.meta_all()
with open(os.path.join(S.ROOT, "Project.dproj"), "w", encoding="utf-8", newline="\n") as f:
    json.dump({"name": "Siege", "gameModule": "", "windowWidth": 1600, "windowHeight": 900,
               "engineVersion": "0.1.0", "startScene": S.guid("Scenes/AssetPreview.scene"),
               "buildScenes": [S.guid("Scenes/AssetPreview.scene")]}, f, indent=2)

grass = S.pbr_material("Materials/Preview Grass.mat", "Textures/Terrain/grass_ground", tiling=(20, 20))
sc = S.Scene("AssetPreview", 10_000)
sc.settings = {
    "ambientColor": [0.55, 0.6, 0.7], "ambientIntensity": 0.35, "bloom": True, "bloomIntensity": 0.4,
    "bloomThreshold": 2.0, "exposure": 1.0, "fxaa": False, "taa": True, "ssao": True, "ssaoIntensity": 1.2,
    "ssaoRadius": 0.6, "tonemapping": True, "skyMode": 1, "skyIntensity": 1.0, "skyRotation": 0.0,
    "skyTexture": S.guid("Sky/alps_field_2k.hdr"), "vignette": 0.2, "gravity": [0, -9.81, 0],
}
sc.add("Main Camera", (0, 2.2, 9.5), (-0.12, 0, 0), Camera={
    "Projection": 0, "Field Of View": 50, "Orthographic Size": 5, "Near Clip": 0.1, "Far Clip": 2000,
    "Primary": True, "Background": [0.4, 0.6, 0.85, 1], "Clear Flags": 1})
sc.add("Sun", (0, 20, 0), (-0.75, 0.6, 0), Directional_Light={
    "Color": [1.0, 0.95, 0.88], "Intensity": 3.0, "Soft Shadows": True, "Light Angle": 1.5})
sc.add("Ground", (0, -0.5, 0), scale=(60, 1, 60), Mesh_Renderer=S.mesh_renderer(1, grass))

ual1 = S.guid("Models/Animations/UAL1_Standard.glb")
ual2 = S.guid("Models/Animations/UAL2_Standard.glb")
body = S.guid("Models/Characters/Superhero_Male_FullBody.gltf")
for i, (clip, src) in enumerate([("Sword_Regular_Combo", ual2), ("Walk_Loop", ual1), ("Idle_Shield_Loop", ual2)]):
    sc.add(f"Soldier {clip}", (-3.0 + i * 1.3, 0, 1.0), Mesh_Renderer=S.mesh_renderer(body),
           Animator=S.animator(clip, src))
sc.add("Horse", (3.2, 0, 0.5), (0, -1.2, 0), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Horse/Horse.gltf")),
       Animator=S.animator("Gallop"))
sc.add("Kite Shield", (0.5, 0, 2.5), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Props/kite_shield/kite_shield.gltf")))
sc.add("Barrels", (-5.5, 0, 0), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Props/wooden_barrels_01/wooden_barrels_01.gltf")))
sc.add("Rock Face", (-7, 0, -6), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Nature/rock_face_01/rock_face_01.gltf")))
sc.add("Boulder", (6, 0, -4), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Nature/boulder_01/boulder_01.gltf")))
sc.add("Fir", (8, 0, -9), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Nature/fir_sapling/fir_sapling.gltf")))
sc.add("Mossy Rocks", (-2, 0, -3), Mesh_Renderer=S.mesh_renderer(S.guid("Models/Nature/rock_moss_set_01/rock_moss_set_01.gltf")))
print("scene", sc.save("Scenes/AssetPreview.scene"))
