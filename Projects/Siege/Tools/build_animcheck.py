"""Scenes/AnimCheck.scene: one soldier kind playing one clip, copies side by side at different speeds, so a single
frame one second in shows the clip at several moments (a check of authored clips: anim_author.py).

python build_animcheck.py <Kind> <Clip | Clip,Clip,...> [loop=0|1] [camera=x,y,z,pitchDeg,yawDeg] [speeds=0.2,0.5,...]
Then: Tools/shot.sh AnimCheck out.png 30 with EXTRA="--fixed-dt 0.0333" (frame 30 = one second)."""
import math
import sys

import siege as S

kind, clip = sys.argv[1], sys.argv[2]
opts = dict(arg.split("=", 1) for arg in sys.argv[3:])
loop = opts.get("loop", "0") == "1"
speeds = [float(v) for v in opts.get("speeds", "0.15,0.35,0.6,0.85,1.0,1.2,1.5,1.9").split(",")]
anims = S.guid("Models/Animations/Soldier_Anims.gltf")
grass = S.guid("Materials/Preview Grass.mat")
sc = S.Scene("AnimCheck", 21_000)
sc.settings = {
    "ambientColor": [0.55, 0.6, 0.7], "ambientIntensity": 0.4, "bloom": False, "exposure": 1.0, "taa": False,
    "ssao": True, "ssaoIntensity": 1.0, "ssaoRadius": 0.5, "tonemapping": True, "skyMode": 1, "skyIntensity": 1.0,
    "skyTexture": S.guid("Sky/kloofendal_48d_partly_cloudy_puresky_2k.hdr"), "gravity": [0, -9.81, 0],
}
spacing = 1.6
width = (len(speeds) - 1) * spacing
x, y, z, pitch, yaw = (float(v) for v in opts.get("camera", f"{width / 2},1.4,{width * 0.75 + 3},-6,0").split(","))
sc.add("Main Camera", (x, y, z), (math.radians(pitch), math.radians(yaw), 0), Camera={
    "Projection": 0, "Field Of View": 40, "Orthographic Size": 5, "Near Clip": 0.05, "Far Clip": 1000,
    "Primary": True, "Background": [0.4, 0.6, 0.85, 1], "Clear Flags": 1})
sc.add("Sun", (0, 20, 0), (-0.8, 0.6, 0), Directional_Light={
    "Color": [1.0, 0.95, 0.88], "Intensity": 3.0, "Soft Shadows": True, "Light Angle": 1.5})
sc.add("Ground", (0, -0.5, 0), scale=(80, 1, 80), Mesh_Renderer=S.mesh_renderer(1, grass))
names = clip.split(",")
if len(names) > 1:  # several clips, one figure each, at full speed
    speeds = [1.0] * len(names)
    spacing = 1.6
for i, speed in enumerate(speeds):
    # Turned to show their right side (the camera looks down -Z; they face +X).
    sc.add(f"{kind} {speed}", (i * spacing, 0, 0), (0, math.radians(float(opts.get("face", "90"))), 0),
           Mesh_Renderer=S.mesh_renderer(S.guid(f"Models/Soldiers/Defenders_{kind}.gltf")),
           Animator=S.animator(names[i] if len(names) > 1 else clip, anims, speed=speed, loop=loop or len(names) > 1))
sc.save("Scenes/AnimCheck.scene")
print("t =", ", ".join(f"{s:.2f}" for s in speeds), "s at one second")
