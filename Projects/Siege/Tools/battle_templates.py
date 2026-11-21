"""The inactive "Template ..." objects the battle script copies (Scripts/Battle.cs): soldiers, horse, arrow,
catapult, the burning shot and its impact, and the Battle object itself. Added to Siege.scene by build_siege.py."""
import siege as S

HIDDEN = (0.0, -300.0, 0.0)
FIRE = S.guid("Textures/FX/fire.png")
SMOKE = S.guid("Textures/FX/smoke.png")


def particles(**kw):
    """A Particle System with quiet defaults; keyword names use underscores for spaces."""
    p = {"Duration": 5.0, "Looping": True, "Play On Awake": True, "Start Rotation": [0, 360], "Angular Velocity": [-40, 40],
         "Max Particles": 1000, "Simulation Space": 1, "Burst Count": 0, "Shape": 1, "Box Size": [1, 1, 1], "Texture": 0,
         "Sorting Order": 0, "Drag": 0.0, "Start Lifetime": [1, 1], "Start Speed": [0, 0], "Start Size": [1, 1],
         "Start Color A": [1, 1, 1, 1], "Start Color B": [1, 1, 1, 1], "Gravity Modifier": 0.0, "Rate Over Time": 10,
         "Shape Angle": 25, "Shape Radius": 0.3, "Blend": 0, "Color Over Lifetime": [[0, 1, 1, 1, 1], [1, 1, 1, 1, 0]],
         "Size Over Lifetime": [[0, 1.0], [1, 1.0]], "Noise": False, "Noise Strength": 0.3, "Noise Frequency": 1.0,
         "Noise Scroll Speed": 1.0, "Lit": False}
    for k, v in kw.items():
        p[k.replace("_", " ")] = v
    return p


HORSE_COATS = ("Bay", "Chestnut", "Black", "Grey")


def add(sc):
    anims = S.guid("Models/Animations/Soldier_Anims.gltf")
    for side in ("Defenders", "Attackers"):
        for kind in ("Swordsman", "Spearman", "Archer", "Knight"):
            sc.add(f"Template {side}_{kind}", HIDDEN, active=False,
                   Mesh_Renderer=S.mesh_renderer(S.guid(f"Models/Soldiers/{side}_{kind}.gltf")),
                   Animator=S.animator("Idle_Shield_Loop", anims),
                   LOD_Group={"LOD 1 Below": 0.12, "LOD 2 Below": 0.05, "LOD 3 Below": 0.02, "Cull Below": 0.0025,
                              "Size": 1.8, "Bias": 1.0, "Fade Width": 0.1, "Generated Levels": 3, "Reduction": 0.35})
    # The horses: a saddle cloth in each side's colours, several coats (Tools/build_horse.py).
    for side in ("Defenders", "Attackers"):
        for coat in HORSE_COATS:
            sc.add(f"Template Horse {side} {coat}", HIDDEN, scale=(0.55, 0.55, 0.55), active=False,
                   Mesh_Renderer=S.mesh_renderer(S.guid(f"Models/Horse/Horse_{side}_{coat}.gltf")),
                   Animator=S.animator("Idle", 0),
                   LOD_Group={"LOD 1 Below": 0.1, "LOD 2 Below": 0.04, "LOD 3 Below": 0.02, "Cull Below": 0.003,
                              "Size": 2.6, "Bias": 1.0, "Fade Width": 0.1, "Generated Levels": 2, "Reduction": 0.4})
    sc.add("Template Arrow", HIDDEN, active=False,
           Mesh_Renderer=S.mesh_renderer(S.guid("Models/Props/Arrow.gltf"), shadows=False))
    sc.add("Template Catapult", HIDDEN, active=False,
           Mesh_Renderer=S.mesh_renderer(S.guid("Models/Siege/Catapult_Frame.gltf")))
    sc.add("Template Catapult Arm", HIDDEN, active=False,
           Mesh_Renderer=S.mesh_renderer(S.guid("Models/Siege/Catapult_Arm.gltf")))

    # The burning stone: flames streaming off it (world space, so they trail behind), a fire streak, its light, and
    # a thick smoke trail from a child.
    shot = sc.add("Template Shot", HIDDEN, scale=(5.5, 5.5, 5.5), active=False,
                  Mesh_Renderer=S.mesh_renderer(S.guid("Models/Nature/stone_01/stone_01.gltf")),
                  Point_Light={"Color": [1.0, 0.55, 0.2], "Intensity": 40.0, "Range": 32.0, "Cast Shadows": False,
                               "Shadow Bias": 0.05},
                  Particle_System=particles(Texture=FIRE, Blend=1, Rate_Over_Time=220, Max_Particles=600,
                                            Start_Lifetime=[0.25, 0.6], Start_Speed=[0.3, 1.5], Start_Size=[0.9, 1.8],
                                            Shape=1, Shape_Radius=0.35, Gravity_Modifier=-0.15,
                                            Start_Color_A=[1.0, 0.85, 0.45, 1.0], Start_Color_B=[1.0, 0.5, 0.15, 1.0],
                                            Color_Over_Lifetime=[[0, 1, 1, 1, 1], [0.5, 1, 0.6, 0.25, 0.8], [1, 0.6, 0.15, 0.05, 0]],
                                            Size_Over_Lifetime=[[0, 0.8], [0.4, 1.2], [1, 0.3]], Noise=True,
                                            Noise_Strength=0.6, Noise_Frequency=1.5, Noise_Scroll_Speed=2.0),
                  Trail_Renderer={"Time": 0.9, "Min Vertex Distance": 0.4, "Width": 1.1, "Width Curve": [[0.0, 1.0], [1.0, 0.1]],
                                  "Color": [[0.0, 1.0, 0.7, 0.3, 0.9], [0.35, 1.0, 0.35, 0.08, 0.5], [1.0, 0.3, 0.1, 0.05, 0.0]],
                                  "Emitting": True, "Texture": 0, "Texture Mode": 0, "Blend": 1, "Sorting Order": 1})
    sc.add("Smoke", parent=shot, scale=(1 / 5.5, 1 / 5.5, 1 / 5.5),
           Particle_System=particles(Texture=SMOKE, Blend=0, Rate_Over_Time=70, Max_Particles=700,
                                     Start_Lifetime=[2.5, 4.5], Start_Speed=[0.2, 0.8], Start_Size=[1.2, 2.0],
                                     Shape=1, Shape_Radius=0.4, Gravity_Modifier=-0.03, Drag=0.4,
                                     Start_Color_A=[0.22, 0.2, 0.19, 0.75], Start_Color_B=[0.32, 0.3, 0.28, 0.6],
                                     Color_Over_Lifetime=[[0, 1, 1, 1, 0.0], [0.08, 1, 1, 1, 1], [1, 1, 1, 1, 0]],
                                     Size_Over_Lifetime=[[0, 0.6], [1, 3.2]], Noise=True, Noise_Strength=0.5,
                                     Noise_Frequency=0.5, Noise_Scroll_Speed=0.6, Lit=True))

    # The impact: earth and flame thrown up, a rolling cloud of smoke, fires burning on in the crater.
    impact = sc.add("Template Impact", HIDDEN, active=False,
                    Particle_System=particles(Texture=SMOKE, Blend=0, Looping=False, Duration=0.2, Rate_Over_Time=0,
                                              Burst_Count=90, Max_Particles=200, Start_Lifetime=[1.0, 2.4],
                                              Start_Speed=[7, 17], Start_Size=[0.4, 1.1], Shape=0, Shape_Angle=38,
                                              Shape_Radius=1.0, Gravity_Modifier=1.0,
                                              Start_Color_A=[0.3, 0.24, 0.17, 1.0], Start_Color_B=[0.18, 0.14, 0.1, 1.0],
                                              Color_Over_Lifetime=[[0, 1, 1, 1, 1], [1, 1, 1, 1, 0]],
                                              Size_Over_Lifetime=[[0, 0.7], [1, 1.6]], Lit=True))
    sc.add("Blast", parent=impact,
           Particle_System=particles(Texture=FIRE, Blend=1, Looping=False, Duration=0.2, Rate_Over_Time=0, Burst_Count=60,
                                     Max_Particles=120, Start_Lifetime=[0.35, 0.9], Start_Speed=[3, 9], Start_Size=[1.5, 3.2],
                                     Shape=1, Shape_Radius=1.2, Gravity_Modifier=-0.2,
                                     Start_Color_A=[1.0, 0.85, 0.5, 1.0], Start_Color_B=[1.0, 0.45, 0.12, 1.0],
                                     Color_Over_Lifetime=[[0, 1, 1, 1, 1], [1, 0.7, 0.2, 0.05, 0]],
                                     Size_Over_Lifetime=[[0, 0.6], [1, 1.5]]))
    sc.add("Smoke", parent=impact,
           Particle_System=particles(Texture=SMOKE, Blend=0, Looping=False, Duration=3.0, Rate_Over_Time=14, Burst_Count=14,
                                     Max_Particles=120, Start_Lifetime=[4.0, 7.0], Start_Speed=[1, 3.5], Start_Size=[3, 5],
                                     Shape=0, Shape_Angle=30, Shape_Radius=1.5, Gravity_Modifier=-0.04, Drag=0.5,
                                     Start_Color_A=[0.2, 0.18, 0.16, 0.8], Start_Color_B=[0.35, 0.32, 0.3, 0.7],
                                     Color_Over_Lifetime=[[0, 1, 1, 1, 0], [0.06, 1, 1, 1, 1], [1, 1, 1, 1, 0]],
                                     Size_Over_Lifetime=[[0, 0.5], [1, 2.6]], Noise=True, Noise_Strength=0.4,
                                     Noise_Frequency=0.4, Noise_Scroll_Speed=0.5, Lit=True))
    sc.add("Embers", parent=impact,
           Particle_System=particles(Texture=FIRE, Blend=1, Looping=False, Duration=6.0, Rate_Over_Time=30, Max_Particles=200,
                                     Start_Lifetime=[0.5, 1.1], Start_Speed=[0.4, 1.6], Start_Size=[0.6, 1.4], Shape=1,
                                     Shape_Radius=2.0, Gravity_Modifier=-0.3,
                                     Start_Color_A=[1.0, 0.7, 0.3, 0.9], Start_Color_B=[1.0, 0.4, 0.1, 0.8],
                                     Color_Over_Lifetime=[[0, 1, 1, 1, 1], [1, 0.6, 0.15, 0.05, 0]],
                                     Size_Over_Lifetime=[[0, 1.0], [1, 0.3]], Noise=True, Noise_Strength=0.5),
           Point_Light={"Color": [1.0, 0.5, 0.18], "Intensity": 14.0, "Range": 14.0, "Cast Shadows": False, "Shadow Bias": 0.05})
    sc.add("Template Flash", HIDDEN, active=False,
           Point_Light={"Color": [1.0, 0.62, 0.3], "Intensity": 160.0, "Range": 55.0, "Cast Shadows": False, "Shadow Bias": 0.05})

    sc.add("Battle", Script={"Scripts": [{"Class": "Siege.Battle", "Fields": {"ArmySize": 320, "CavalryPerSide": 24,
                                                                              "Catapults": 4, "Seed": 7},
                                          "Enabled": True}]})
