"""Scenes/Lineup.scene: every soldier kind of both sides in a row, animating (a check of the soldier models)."""
import sys
import siege as S

clips = dict(arg.split("=", 1) for arg in sys.argv[1:])
ual1 = S.guid("Models/Animations/UAL1_Standard.glb")
ual2 = S.guid("Models/Animations/UAL2_Standard.glb")
CLIP_SOURCE = {"Walk_Loop": ual1, "Idle_Loop": ual1, "Sword_Attack": ual1, "Jog_Fwd_Loop": ual1, "Death01": ual1,
               "Pistol_Aim_Neutral": ual1, "Spell_Simple_Shoot": ual1, "A_TPose": ual1}
grass = S.guid("Materials/Preview Grass.mat")
sc = S.Scene("Lineup", 20_000)
sc.settings = {
    "ambientColor": [0.55, 0.6, 0.7], "ambientIntensity": 0.35, "bloom": True, "bloomIntensity": 0.3,
    "bloomThreshold": 2.0, "exposure": 1.0, "taa": True, "ssao": True, "ssaoIntensity": 1.2, "ssaoRadius": 0.5,
    "tonemapping": True, "skyMode": 1, "skyIntensity": 1.0, "skyTexture": S.guid("Sky/alps_field_2k.hdr"),
    "vignette": 0.15, "gravity": [0, -9.81, 0],
}
cam = clips.pop("camera", "0,1.3,5.2,-0.05")
x, y, z, pitch = (float(v) for v in cam.split(","))
sc.add("Main Camera", (x, y, z), (pitch, 0, 0), Camera={
    "Projection": 0, "Field Of View": 45, "Orthographic Size": 5, "Near Clip": 0.05, "Far Clip": 1000,
    "Primary": True, "Background": [0.4, 0.6, 0.85, 1], "Clear Flags": 1})
sc.add("Sun", (0, 20, 0), (-0.7, 0.5, 0), Directional_Light={
    "Color": [1.0, 0.95, 0.88], "Intensity": 3.0, "Soft Shadows": True, "Light Angle": 1.5})
sc.add("Ground", (0, -0.5, 0), scale=(60, 1, 60), Mesh_Renderer=S.mesh_renderer(1, grass))
default = {"Swordsman": "Sword_Regular_Combo", "Spearman": "Idle_Shield_Loop", "Archer": "Idle_Loop", "Knight": "Walk_Loop"}
i = 0
for side in ("Defenders", "Attackers"):
    for kind in ("Swordsman", "Spearman", "Archer", "Knight"):
        clip = clips.get(kind, default[kind])
        sc.add(f"{side} {kind}", (-3.5 + i * 1.0, 0, 0), Mesh_Renderer=S.mesh_renderer(S.guid(f"Models/Soldiers/{side}_{kind}.gltf")),
               Animator=S.animator(clip, CLIP_SOURCE.get(clip, ual2)))
        i += 1
sc.save("Scenes/Lineup.scene")
