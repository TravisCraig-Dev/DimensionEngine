"""Scenes/RideCheck.scene: knights riding, seated as the battle seats them (Scripts/RideCheck.cs, Mount), side by side
and seen from their right: on a standing horse (Ride_Idle), at a walk, at the gallop (Ride_Gallop) and cutting down
from the gallop (Ride_Slash), each pair a little apart in time. A check of the riding clips (anim_author.py).

python build_ridecheck.py [camera=x,y,z,pitchDeg,yawDeg] [face=deg] [slash=speed]
Then: Tools/shot.sh RideCheck out.png 30 with EXTRA="--fixed-dt 0.0333"."""
import math
import sys

import siege as S

opts = dict(arg.split("=", 1) for arg in sys.argv[1:])
anims = S.guid("Models/Animations/Soldier_Anims.gltf")
grass = S.guid("Materials/Preview Grass.mat")
sc = S.Scene("RideCheck", 22_000)
sc.settings = {
    "ambientColor": [0.55, 0.6, 0.7], "ambientIntensity": 0.4, "bloom": False, "exposure": 1.0, "taa": False,
    "ssao": True, "ssaoIntensity": 1.0, "ssaoRadius": 0.5, "tonemapping": True, "skyMode": 1, "skyIntensity": 1.0,
    "skyTexture": S.guid("Sky/kloofendal_48d_partly_cloudy_puresky_2k.hdr"), "gravity": [0, -9.81, 0],
}
pairs = [("Idle", "Ride_Idle", 1.0), ("Walk", "Ride_Idle", 1.0), ("Gallop", "Ride_Gallop", 1.0),
         ("Gallop", "Ride_Slash", float(opts.get("slash", "1.0")))]
spacing = 3.2
width = (len(pairs) - 1) * spacing
x, y, z, pitch, yaw = (float(v) for v in opts.get("camera", f"{width / 2},1.9,{width * 0.8 + 5},-6,0").split(","))
face = float(opts.get("face", "90"))  # their right side to the camera (they face +X)
sc.add("Main Camera", (x, y, z), (math.radians(pitch), math.radians(yaw), 0), Camera={
    "Projection": 0, "Field Of View": 40, "Orthographic Size": 5, "Near Clip": 0.05, "Far Clip": 1000,
    "Primary": True, "Background": [0.4, 0.6, 0.85, 1], "Clear Flags": 1})
sc.add("Sun", (0, 20, 0), (-0.8, 0.6, 0), Directional_Light={
    "Color": [1.0, 0.95, 0.88], "Intensity": 3.0, "Soft Shadows": True, "Light Angle": 1.5})
sc.add("Ground", (0, -0.5, 0), scale=(80, 1, 80), Mesh_Renderer=S.mesh_renderer(1, grass))
for i, (horse_clip, rider_clip, speed) in enumerate(pairs):
    name = f"Horse {i} {horse_clip}"
    sc.add(name, (i * spacing, 0, 0), (0, math.radians(face), 0), scale=(0.55, 0.55, 0.55),
           Mesh_Renderer=S.mesh_renderer(S.guid("Models/Horse/Horse_Defenders_Bay.gltf")),
           Animator=S.animator(horse_clip, 0, loop=True))
    sc.add(f"Rider {i} {rider_clip}", (i * spacing, 0, 0), (0, math.radians(face), 0),
           Mesh_Renderer=S.mesh_renderer(S.guid("Models/Soldiers/Defenders_Knight.gltf")),
           Animator=S.animator(rider_clip, anims, speed=speed, loop=True),
           Script={"Scripts": [{"Class": "Siege.RideCheck", "Fields": {"Horse": name, "Yaw": face}, "Enabled": True}]})
sc.save("Scenes/RideCheck.scene")
